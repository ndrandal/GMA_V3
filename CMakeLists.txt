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

# ---- Dependencies: Boost (Beast uses Asio inside Boost), Threads ----
find_package(Threads REQUIRED)
find_package(Boost 1.75 REQUIRED COMPONENTS system)

# ---- RapidJSON (header-only) ----
# If you have a third_party/rapidjson include, prefer it; else rely on system include paths.
set(RAPIDJSON_ROOT_HINT "${CMAKE_SOURCE_DIR}/third_party/rapidjson")
if (EXISTS "${RAPIDJSON_ROOT_HINT}/include/rapidjson")
  add_library(RapidJSON::rapidjson INTERFACE IMPORTED)
  target_include_directories(RapidJSON::rapidjson INTERFACE "${RAPIDJSON_ROOT_HINT}/include")
else()
  # Fallback: create an interface target; include dirs rely on compiler's default paths
  add_library(RapidJSON::rapidjson INTERFACE IMPORTED)
endif()

# ---- Sources ----
# We intentionally glob to make this resilient while refactoring.
# If you prefer explicit lists later, you can replace this with target_sources.
file(GLOB_RECURSE GMA_SOURCES
  "${CMAKE_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE GMA_PUBLIC_HEADERS
  "${CMAKE_SOURCE_DIR}/include/**/*.hpp"
  "${CMAKE_SOURCE_DIR}/include/*.hpp"
)

add_library(gma STATIC ${GMA_SOURCES} ${GMA_PUBLIC_HEADERS})
target_include_directories(gma PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
)
target_link_libraries(gma
  PUBLIC
    Boost::system
    Threads::Threads
    RapidJSON::rapidjson
)

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

# ---- Example app (optional) ----
# If you have a main() somewhere (e.g., apps/server_main.cpp), you can enable it later.

# ---- Tests (opt-in) ----
if (GMA_BUILD_TESTS)
  enable_testing()
  file(GLOB_RECURSE GMA_TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/*.cpp")
  add_executable(gma_tests ${GMA_TEST_SOURCES})
  target_include_directories(gma_tests PRIVATE "${CMAKE_SOURCE_DIR}/include")
  target_link_libraries(gma_tests PRIVATE gma Boost::system Threads::Threads RapidJSON::rapidjson)
  add_test(NAME gma_tests COMMAND gma_tests)
endif()
