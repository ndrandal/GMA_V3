cmake_minimum_required(VERSION 3.20)
project(GMA_V3 LANGUAGES CXX)

# ---- Options ----
option(GMA_BUILD_TESTS "Build unit tests in tests/" OFF)
option(GMA_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# ---- C++ standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Platform defs (Beast/Asio on Windows) ----
if (WIN32)
  add_definitions(-D_WIN32_WINNT=0x0A00) # Windows 10
endif()

# ---- Third-party root (your local unpacked libs)
#     Override with: -DTHIRD_PARTY_ROOT=C:/wherever
if (NOT DEFINED THIRD_PARTY_ROOT)
  if (WIN32)
    set(THIRD_PARTY_ROOT "C:/thirdparty")
  else()
    set(THIRD_PARTY_ROOT "${CMAKE_SOURCE_DIR}/third_party")
  endif()
endif()

# ---- Threads (always needed)
find_package(Threads REQUIRED)

# ---- Try to find Boost normally first
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.75 QUIET COMPONENTS system)

set(USE_HEADER_ONLY_BOOST OFF)
if (NOT Boost_FOUND)
  # Fallback to header-only mode using your local folders
  set(BOOST_FALLBACK_INCLUDE
      "${THIRD_PARTY_ROOT}/boost_1_88_0")
  if (EXISTS "${BOOST_FALLBACK_INCLUDE}/boost/beast/core.hpp")
    message(STATUS "Using header-only Boost from: ${BOOST_FALLBACK_INCLUDE}")
    set(USE_HEADER_ONLY_BOOST ON)

    # Create dummy INTERFACE targets so downstream link lines still work
    add_library(Boost::headers INTERFACE IMPORTED)
    target_include_directories(Boost::headers INTERFACE "${BOOST_FALLBACK_INCLUDE}")

    add_library(Boost::system INTERFACE IMPORTED) # header-only error_code
    target_link_libraries(Boost::system INTERFACE Boost::headers)

  else()
    message(FATAL_ERROR
      "Boost not found and no fallback at ${BOOST_FALLBACK_INCLUDE}. "
      "Install Boost or set -DTHIRD_PARTY_ROOT=C:/thirdparty")
  endif()
else()
  message(STATUS "Found Boost: ${Boost_INCLUDE_DIRS}")
endif()

# ---- RapidJSON (header-only)
# Prefer your local C:/thirdparty/rapidjson/include if present
set(RAPIDJSON_FALLBACK_INCLUDE "${THIRD_PARTY_ROOT}/rapidjson/include")
add_library(RapidJSON::rapidjson INTERFACE IMPORTED)
if (EXISTS "${RAPIDJSON_FALLBACK_INCLUDE}/rapidjson/document.h")
  target_include_directories(RapidJSON::rapidjson INTERFACE "${RAPIDJSON_FALLBACK_INCLUDE}")
else()
  # Let compiler/system include provide it if installed
  message(STATUS "Using system RapidJSON includes")
endif()

# ---- Sources (library) ----
# Build the library from all sources EXCEPT main.cpp
set(MAIN_SRC "${CMAKE_SOURCE_DIR}/src/main.cpp")

file(GLOB_RECURSE GMA_SOURCES "${CMAKE_SOURCE_DIR}/src/*.cpp")
if (EXISTS "${MAIN_SRC}")
  list(REMOVE_ITEM GMA_SOURCES "${MAIN_SRC}")
endif()

file(GLOB_RECURSE GMA_PUBLIC_HEADERS
  "${CMAKE_SOURCE_DIR}/include/**/*.hpp"
  "${CMAKE_SOURCE_DIR}/include/*.hpp"
)

add_library(gma STATIC ${GMA_SOURCES} ${GMA_PUBLIC_HEADERS})
target_include_directories(gma PUBLIC "${CMAKE_SOURCE_DIR}/include")

# If we have a full Boost install use it; else use header-only targets
if (Boost_FOUND)
  target_include_directories(gma PUBLIC ${Boost_INCLUDE_DIRS})
  target_link_libraries(gma PUBLIC Boost::system)
else()
  target_link_libraries(gma PUBLIC Boost::system) # our INTERFACE dummy
  # Header-only mode: no Boost libs required; add safe defines
  target_compile_definitions(gma PUBLIC
    BOOST_ALL_NO_LIB
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED
    BOOST_ASIO_NO_DEPRECATED
    BOOST_BEAST_USE_STD_STRING_VIEW
  )
endif()

target_link_libraries(gma PUBLIC Threads::Threads RapidJSON::rapidjson)

# ---- Warnings ----
if (MSVC)
  target_compile_options(gma PRIVATE /W4)
  if (GMA_WARNINGS_AS_ERRORS)
    target_compile_options(gma PRIVATE /WX)
  endif()
else()
  target_compile_options(gma PRIVATE -Wall -Wextra -Wpedantic)
  if (GMA_WARNINGS_AS_ERRORS)
    target_compile_options(gma PRIVATE -Werror)
  endif()
endif()

# ---- App executable (uses your existing src/main.cpp) ----
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
endif()

# ---- Tests (opt-in) ----
if (GMA_BUILD_TESTS)
  enable_testing()
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
  target_link_libraries(gma_tests PRIVATE Threads::Threads RapidJSON::rapidjson)
  add_test(NAME gma_tests COMMAND gma_tests)
endif()
