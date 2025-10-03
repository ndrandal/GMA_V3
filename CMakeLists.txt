# File: CMakeLists.txt (drop-in)
cmake_minimum_required(VERSION 3.18)
project(GMA_V3 LANGUAGES CXX)

# -------- Basics --------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS  "Build tests"            OFF)   # Per user: ignore tests
option(BUILD_SERVER "Build server executable" ON)

# -------- Dependencies --------
find_package(Threads REQUIRED)

# Boost: need at least System (Beast depends on it). We intentionally don't require headers-only components.
# If your system Boost is older than 1.70, upgrade (Beast is bundled with Boost).
find_package(Boost 1.70 REQUIRED COMPONENTS system)

# RapidJSON (header-only). Try package first, else fall back to a local vendor/rapidjson include.
find_path(RAPIDJSON_INCLUDE_DIR rapidjson/document.h
  HINTS ${RAPIDJSON_ROOT} /usr/include /usr/local/include
)
if (NOT RAPIDJSON_INCLUDE_DIR)
  message(FATAL_ERROR "RapidJSON headers not found. Install rapidjson-dev or set RAPIDJSON_ROOT.")
endif()

# -------- Sources --------
# Pull all non-test .cpp files under src/
file(GLOB_RECURSE GMA_SRC
  CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

# If you want to exclude specific legacy files, do it here, e.g.:
# list(REMOVE_ITEM GMA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/core/ThreadPool.cpp")
# list(REMOVE_ITEM GMA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/core/OrderBookManager.cpp")

# -------- Library --------
add_library(gma STATIC ${GMA_SRC})

target_include_directories(gma
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${RAPIDJSON_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(gma
  PUBLIC
    Threads::Threads
    Boost::system
)

target_compile_definitions(gma
  PRIVATE
    $<$<CONFIG:Debug>:GMA_DEBUG_BUILD=1>
)

# -------- Executable --------
if (BUILD_SERVER)
  add_executable(gma_main src/main.cpp)
  target_link_libraries(gma_main PRIVATE gma)
endif()

# -------- Tests (disabled for D1) --------
if (BUILD_TESTS)
  add_subdirectory(tests)
endif()
