cmake_minimum_required(VERSION 3.15)
project(GMA_V3 LANGUAGES CXX)

# ——————————————————————————————————————————————————————————
# 1. Compiler settings
# ——————————————————————————————————————————————————————————
set(CMAKE_CXX_STANDARD      20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS     OFF)

# ——————————————————————————————————————————————————————————
# 2. Build options
# ——————————————————————————————————————————————————————————
option(BUILD_TESTS "Enable building of unit tests" ON)

# ——————————————————————————————————————————————————————————
# 3. Locate system dependencies
# ——————————————————————————————————————————————————————————
find_package(Threads REQUIRED)

# ——————————————————————————————————————————————————————————
# 4. GoogleTest (for tests only)
# ——————————————————————————————————————————————————————————
if(BUILD_TESTS)
  add_subdirectory(external/googletest-main)
endif()

# ——————————————————————————————————————————————————————————
# 5. Build the 'gma' library
# ——————————————————————————————————————————————————————————
file(GLOB_RECURSE GMA_SOURCES
  "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/*/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/*/*/*.cpp"
)

add_library(gma STATIC ${GMA_SOURCES})

target_include_directories(gma
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    ${PROJECT_SOURCE_DIR}/lib/boost
)

# Link Threads transitively
target_link_libraries(gma
  PUBLIC Threads::Threads
)

# ——————————————————————————————————————————————————————————
# 6. Build the main executable
# ——————————————————————————————————————————————————————————
add_executable(gma_main
  ${PROJECT_SOURCE_DIR}/src/main.cpp
)

target_link_libraries(gma_main
  PRIVATE gma
)

# ——————————————————————————————————————————————————————————
# 7. Wire up tests
# ——————————————————————————————————————————————————————————
if(BUILD_TESTS)
  enable_testing()

  # Force GoogleTest discovery to use cmd.exe instead of pwsh
  set(GTEST_CTEST_RUNNER cmd CACHE STRING "Use cmd for GTest discovery")

  # Add all test modules under tests/
  add_subdirectory(tests)
endif()
