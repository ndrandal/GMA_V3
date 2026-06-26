cmake_minimum_required(VERSION 3.20)
project(GMA_V3 LANGUAGES CXX)

# ---- Options ----
option(GMA_BUILD_TESTS "Build unit tests in tests/" OFF)
option(GMA_BUILD_BENCHMARKS "Build Google Benchmark suite in benchmarks/" OFF)
option(GMA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# Microarchitecture (ENC-790 / H5): -march=native pins the binary to the exact
# build CPU and SIGILLs on older hosts. Release/RelWithDebInfo therefore default
# to a portable baseline (GMA_ARCH_BASELINE, x86-64-v2 ≈ any 2009+ x86_64 CPU);
# -march=native is opt-in via GMA_NATIVE_ARCH. -O3 and LTO are kept regardless.
option(GMA_NATIVE_ARCH
  "Build Release with -march=native (non-portable: pins the binary to the build CPU; SIGILL on other hosts)"
  OFF)
set(GMA_ARCH_BASELINE "x86-64-v2" CACHE STRING
  "Portable -march baseline used for Release on x86_64 when GMA_NATIVE_ARCH is OFF (empty string disables -march entirely)")

# Sanitizers (ENC-801 / M15): opt-in, default off so a plain `cmake .. && make`
# is byte-for-byte unaffected. When set, the flags are injected into compile AND
# link for every target in this build (engine, connectors, server, tests, gtest).
set(GMA_SANITIZE "off" CACHE STRING
  "Enable a sanitizer for all targets: off|address|thread|undefined|address+undefined")
set_property(CACHE GMA_SANITIZE PROPERTY STRINGS off address thread undefined address+undefined)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Platform defs (Beast/Asio on Windows) ----
if (WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00) # Windows 10
endif()

# ---- Third-party root
# Override with: -DTHIRD_PARTY_ROOT=C:/wherever
if (NOT DEFINED THIRD_PARTY_ROOT)
  if (WIN32)
    set(THIRD_PARTY_ROOT "C:/thirdparty")
  else()
    set(THIRD_PARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party")
  endif()
endif()

# ---- Threads
find_package(Threads REQUIRED)

# ---- OpenSSL (optional — enables wss:// in WsFeedClient)
find_package(OpenSSL QUIET)
if (OpenSSL_FOUND)
  message(STATUS "OpenSSL found: ${OPENSSL_VERSION} — wss:// feed support enabled")
else()
  message(STATUS "OpenSSL not found — wss:// feed support disabled (ws:// still works)")
endif()

# =======================
# Sanitizers (opt-in; ENC-801 / M15)
# =======================
# Injected at directory scope BEFORE any target is defined, so every target
# created below (including FetchContent's googletest/benchmark) is instrumented
# in both compile and link. Default "off" => no flags => default build unchanged.
if (NOT GMA_SANITIZE STREQUAL "off")
  if (MSVC)
    message(WARNING "GMA_SANITIZE=${GMA_SANITIZE} is not supported with this MSVC toolchain — ignoring")
  else()
    if (GMA_SANITIZE STREQUAL "address")
      set(_GMA_SAN_FLAGS -fsanitize=address -fno-omit-frame-pointer)
    elseif (GMA_SANITIZE STREQUAL "thread")
      set(_GMA_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    elseif (GMA_SANITIZE STREQUAL "undefined")
      set(_GMA_SAN_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
    elseif (GMA_SANITIZE STREQUAL "address+undefined")
      set(_GMA_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    else()
      message(FATAL_ERROR
        "GMA_SANITIZE='${GMA_SANITIZE}' is invalid — use off|address|thread|undefined|address+undefined")
    endif()
    add_compile_options(${_GMA_SAN_FLAGS})
    add_link_options(${_GMA_SAN_FLAGS})
    message(STATUS "Sanitizer enabled for all targets: ${GMA_SANITIZE}")
  endif()
endif()

# =======================
# Warning flags helper (ENC-806 / L12)
# =======================
# Applies the standard warning set to a target. -Werror stays OFF unless the
# opt-in GMA_WARNINGS_AS_ERRORS is set, so concurrent source edits never break
# the central build just by emitting a warning.
function(gma_apply_warnings tgt)
  if (MSVC)
    target_compile_options(${tgt} PRIVATE /W4)
    if (GMA_WARNINGS_AS_ERRORS)
      target_compile_options(${tgt} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    if (GMA_WARNINGS_AS_ERRORS)
      target_compile_options(${tgt} PRIVATE -Werror)
    endif()
  endif()
endfunction()

# =======================
# Boost (prefer normal install)
# =======================
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.75 QUIET COMPONENTS system)

set(USE_HEADER_ONLY_BOOST OFF)
if (NOT Boost_FOUND)
  # Fallback to header-only using your local unpacked folder
  set(BOOST_FALLBACK_INCLUDE "${THIRD_PARTY_ROOT}/boost_1_88_0")
  if (EXISTS "${BOOST_FALLBACK_INCLUDE}/boost/beast/core.hpp")
    message(STATUS "Using header-only Boost from: ${BOOST_FALLBACK_INCLUDE}")
    set(USE_HEADER_ONLY_BOOST ON)

    add_library(Boost::headers INTERFACE IMPORTED)
    target_include_directories(Boost::headers INTERFACE "${BOOST_FALLBACK_INCLUDE}")

    # Provide a stub 'Boost::system' since <boost/system/error_code.hpp> is header-only
    add_library(Boost::system INTERFACE IMPORTED)
    target_link_libraries(Boost::system INTERFACE Boost::headers)
  else()
    message(FATAL_ERROR
      "Boost not found and no fallback at ${BOOST_FALLBACK_INCLUDE}. "
      "Install Boost or set -DTHIRD_PARTY_ROOT=C:/thirdparty")
  endif()
else()
  message(STATUS "Found Boost: ${Boost_INCLUDE_DIRS}")
endif()

# =======================
# RapidJSON (header-only)
# =======================
# We want #include <rapidjson/document.h>
# Accept both:
#   C:/thirdparty/rapidjson            (contains folder 'rapidjson/')
#   C:/thirdparty/rapidjson/include    (contains folder 'rapidjson/')
set(RAPIDJSON_HINTS
  "${RAPIDJSON_ROOT}"
  "$ENV{RAPIDJSON_ROOT}"
  "${THIRD_PARTY_ROOT}/rapidjson"
  "${THIRD_PARTY_ROOT}/rapidjson/include"
)

find_path(RAPIDJSON_INCLUDE_DIR
  NAMES rapidjson/document.h
  HINTS ${RAPIDJSON_HINTS}
)

if (NOT RAPIDJSON_INCLUDE_DIR)
  message(FATAL_ERROR
    "RapidJSON headers not found. Set RAPIDJSON_ROOT or place them under "
    "${THIRD_PARTY_ROOT}/rapidjson so that ${RAPIDJSON_INCLUDE_DIR}/rapidjson/document.h exists.")
endif()

add_library(RapidJSON::rapidjson INTERFACE IMPORTED)
target_include_directories(RapidJSON::rapidjson INTERFACE "${RAPIDJSON_INCLUDE_DIR}")
message(STATUS "RapidJSON include dir: ${RAPIDJSON_INCLUDE_DIR}")

# =======================
# Sources / targets
# =======================
# Engine / connector split:
#   gma_engine             — domain-agnostic core (all src/**/*.cpp, minus main)
#   gma_connector_market   — market-specific code (connectors/market/**)
#   gma                    — INTERFACE alias linking both (legacy consumer target)
#   gma_server             — composition root, picks up main.cpp + both libs
# The split is enforced by include-directory scoping: gma_engine's PUBLIC
# include paths contain *only* the engine headers, so engine code that
# accidentally references a connector header fails to compile.

set(MAIN_SRC "${CMAKE_SOURCE_DIR}/src/main.cpp")

file(GLOB_RECURSE GMA_ENGINE_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
if (EXISTS "${MAIN_SRC}")
  list(REMOVE_ITEM GMA_ENGINE_SOURCES "${MAIN_SRC}")
endif()

file(GLOB_RECURSE GMA_ENGINE_HEADERS
  "${CMAKE_SOURCE_DIR}/include/**/*.hpp"
  "${CMAKE_SOURCE_DIR}/include/*.hpp"
)

file(GLOB_RECURSE GMA_MARKET_SOURCES "${CMAKE_SOURCE_DIR}/connectors/market/src/*.cpp")
file(GLOB_RECURSE GMA_MARKET_HEADERS
  "${CMAKE_SOURCE_DIR}/connectors/market/include/**/*.hpp"
  "${CMAKE_SOURCE_DIR}/connectors/market/include/*.hpp"
)

# ---- Engine library ----
add_library(gma_engine STATIC ${GMA_ENGINE_SOURCES} ${GMA_ENGINE_HEADERS})
target_include_directories(gma_engine PUBLIC "${CMAKE_SOURCE_DIR}/include")

if (Boost_FOUND)
  target_include_directories(gma_engine PUBLIC ${Boost_INCLUDE_DIRS})
  target_link_libraries(gma_engine PUBLIC Boost::system)
else()
  target_link_libraries(gma_engine PUBLIC Boost::system)
  target_compile_definitions(gma_engine PUBLIC
    BOOST_ALL_NO_LIB
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED
    BOOST_ASIO_NO_DEPRECATED
    BOOST_BEAST_USE_STD_STRING_VIEW
  )
endif()

target_link_libraries(gma_engine PUBLIC Threads::Threads RapidJSON::rapidjson)

if (OpenSSL_FOUND)
  target_link_libraries(gma_engine PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(gma_engine PUBLIC GMA_HAS_SSL=1)
endif()

# ---- Market connector library ----
add_library(gma_connector_market STATIC ${GMA_MARKET_SOURCES} ${GMA_MARKET_HEADERS})
target_include_directories(gma_connector_market PUBLIC
  "${CMAKE_SOURCE_DIR}/connectors/market/include"
)
target_link_libraries(gma_connector_market PUBLIC gma_engine)

# ---- Legacy umbrella alias so existing rules that link `gma` keep working ----
add_library(gma INTERFACE)
target_link_libraries(gma INTERFACE gma_engine gma_connector_market)

# ---- Synthetic connector (demo): proves the zero-core-changes invariant ----
# Not linked into `gma` — kept as an opt-in target that test code and demos
# pull in explicitly. Adding a connector requires (a) this library target and
# (b) one `registerWith` call at the composition root; no engine edits.
file(GLOB_RECURSE GMA_SYNTHETIC_SOURCES
  "${CMAKE_SOURCE_DIR}/connectors/synthetic/src/*.cpp")
file(GLOB_RECURSE GMA_SYNTHETIC_HEADERS
  "${CMAKE_SOURCE_DIR}/connectors/synthetic/include/**/*.hpp"
  "${CMAKE_SOURCE_DIR}/connectors/synthetic/include/*.hpp")
add_library(gma_connector_synthetic STATIC
  ${GMA_SYNTHETIC_SOURCES} ${GMA_SYNTHETIC_HEADERS})
target_include_directories(gma_connector_synthetic PUBLIC
  "${CMAKE_SOURCE_DIR}/connectors/synthetic/include")
target_link_libraries(gma_connector_synthetic PUBLIC gma_engine)

# Warnings — apply to every first-party target (ENC-806 / L12). Previously only
# gma_engine + gma_connector_market got -Wall/-Wextra; gma_connector_synthetic,
# gma_server, gma_tests and the benchmarks were silent. gma_server/gma_tests/
# benchmarks are wired at their definition sites below.
gma_apply_warnings(gma_engine)
gma_apply_warnings(gma_connector_market)
gma_apply_warnings(gma_connector_synthetic)

if (WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00 -DNOMINMAX)
endif()

# =======================
# Release optimizations
# =======================
if (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  if (MSVC)
    target_compile_options(gma_engine           PRIVATE /O2 /Ob2 /GL)
    target_compile_options(gma_connector_market PRIVATE /O2 /Ob2 /GL)
    target_link_options(gma_engine           PUBLIC /LTCG)
    target_link_options(gma_connector_market PUBLIC /LTCG)
  else()
    # Microarchitecture (ENC-790 / H5). -O3 always; -march is portable by default.
    set(GMA_RELEASE_ARCH_FLAGS "")
    if (GMA_NATIVE_ARCH)
      list(APPEND GMA_RELEASE_ARCH_FLAGS -march=native)
      message(STATUS "Release: -march=native (GMA_NATIVE_ARCH=ON) — binary pinned to THIS CPU")
    elseif (NOT GMA_ARCH_BASELINE STREQUAL "" AND
            CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
      list(APPEND GMA_RELEASE_ARCH_FLAGS "-march=${GMA_ARCH_BASELINE}")
      message(STATUS "Release: portable -march=${GMA_ARCH_BASELINE} (set GMA_NATIVE_ARCH=ON to target the build CPU)")
    else()
      message(STATUS "Release: no -march (portable default)")
    endif()
    target_compile_options(gma_engine           PRIVATE -O3 ${GMA_RELEASE_ARCH_FLAGS})
    target_compile_options(gma_connector_market PRIVATE -O3 ${GMA_RELEASE_ARCH_FLAGS})
  endif()

  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
  if (ipo_supported)
    set_property(TARGET gma_engine           PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET gma_connector_market PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    if (TARGET gma_server)
      set_property(TARGET gma_server PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
    message(STATUS "LTO enabled for gma_engine + gma_connector_market + gma_server")
  else()
    message(STATUS "LTO not supported: ${ipo_output}")
  endif()
endif()

# App executable (if src/main.cpp exists)
if (EXISTS "${MAIN_SRC}")
  add_executable(gma_server "${MAIN_SRC}")
  target_include_directories(gma_server PRIVATE "${CMAKE_SOURCE_DIR}/include")
  if (Boost_FOUND)
    target_include_directories(gma_server PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(gma_server PRIVATE gma Boost::system)
  else()
    target_link_libraries(gma_server PRIVATE gma Boost::system)
    target_compile_definitions(gma_server PRIVATE
      BOOST_ALL_NO_LIB
      BOOST_ERROR_CODE_HEADER_ONLY
      BOOST_SYSTEM_NO_DEPRECATED
      BOOST_ASIO_NO_DEPRECATED
      BOOST_BEAST_USE_STD_STRING_VIEW
    )
  endif()
  target_link_libraries(gma_server PRIVATE Threads::Threads RapidJSON::rapidjson)
  gma_apply_warnings(gma_server)
endif()

# Tests (opt-in)
if (GMA_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  file(GLOB_RECURSE GMA_TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  add_executable(gma_tests ${GMA_TEST_SOURCES})
  target_include_directories(gma_tests PRIVATE "${CMAKE_SOURCE_DIR}/include")
  if (Boost_FOUND)
    target_include_directories(gma_tests PRIVATE ${Boost_INCLUDE_DIRS})
    target_link_libraries(gma_tests PRIVATE gma Boost::system)
  else()
    target_link_libraries(gma_tests PRIVATE gma Boost::system)
    target_compile_definitions(gma_tests PRIVATE
      BOOST_ALL_NO_LIB
      BOOST_ERROR_CODE_HEADER_ONLY
      BOOST_SYSTEM_NO_DEPRECATED
      BOOST_ASIO_NO_DEPRECATED
      BOOST_BEAST_USE_STD_STRING_VIEW
    )
  endif()
  target_link_libraries(gma_tests PRIVATE
    Threads::Threads
    RapidJSON::rapidjson
    GTest::gtest_main
    gma_connector_synthetic
  )
  gma_apply_warnings(gma_tests)

  # CorpusTest data (ENC-807 / L17): copy the corpus next to the test binary and
  # run the test from that directory, so CorpusTest can always locate
  # corpus_requests.json (it otherwise GTEST_SKIPs / fails on the missing file).
  add_custom_command(TARGET gma_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_SOURCE_DIR}/tests/treebuilder/corpus_requests.json"
      "$<TARGET_FILE_DIR:gma_tests>/corpus_requests.json"
    COMMENT "Copying corpus_requests.json next to gma_tests")

  add_test(NAME gma_tests
    COMMAND gma_tests
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:gma_tests>")
endif()

# Benchmarks (opt-in)
if (GMA_BUILD_BENCHMARKS)
  include(FetchContent)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
  )
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googlebenchmark)

  # Helper: create a benchmark executable from a single source file
  function(gma_add_benchmark NAME SRC)
    add_executable(${NAME} ${SRC})
    target_include_directories(${NAME} PRIVATE "${CMAKE_SOURCE_DIR}/include")
    if (Boost_FOUND)
      target_include_directories(${NAME} PRIVATE ${Boost_INCLUDE_DIRS})
      target_link_libraries(${NAME} PRIVATE gma Boost::system)
    else()
      target_link_libraries(${NAME} PRIVATE gma Boost::system)
      target_compile_definitions(${NAME} PRIVATE
        BOOST_ALL_NO_LIB
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_SYSTEM_NO_DEPRECATED
        BOOST_ASIO_NO_DEPRECATED
        BOOST_BEAST_USE_STD_STRING_VIEW
      )
    endif()
    target_link_libraries(${NAME} PRIVATE
      Threads::Threads
      RapidJSON::rapidjson
      benchmark::benchmark
    )
    gma_apply_warnings(${NAME})
  endfunction()

  gma_add_benchmark(bench_atomic_functions  "${CMAKE_SOURCE_DIR}/benchmarks/AtomicFunctionsBench.cpp")
  gma_add_benchmark(bench_dispatcher        "${CMAKE_SOURCE_DIR}/benchmarks/DispatcherBench.cpp")
  gma_add_benchmark(bench_atomic_store      "${CMAKE_SOURCE_DIR}/benchmarks/AtomicStoreBench.cpp")
  gma_add_benchmark(bench_thread_pool       "${CMAKE_SOURCE_DIR}/benchmarks/ThreadPoolBench.cpp")
  gma_add_benchmark(bench_window_nodes      "${CMAKE_SOURCE_DIR}/benchmarks/WindowNodesBench.cpp")

  # Convenience target: build all benchmarks at once
  add_custom_target(gma_benchmarks DEPENDS
    bench_atomic_functions
    bench_dispatcher
    bench_atomic_store
    bench_thread_pool
    bench_window_nodes
  )
endif()
