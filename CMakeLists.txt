# Rewritten CMakeLists.txt for GMA_V3 (main project)

cmake_minimum_required(VERSION 3.18)  # Using a newer CMake for modern features
project(GMA_V3 LANGUAGES CXX)

# 1. Project-wide settings
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Enable building of unit tests" ON)
option(BUILD_SERVER "Build WebSocket server component" ON)

# 2. Dependencies
find_package(Threads REQUIRED)

# Find Boost (for Beast/Asio). We require headers and possibly Boost.System
# If Boost is not installed, we'll fall back to header-only mode using project lib.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.70 COMPONENTS system REQUIRED)  # Require Boost.System
# Note: If Boost is header-only (Beast, Asio) and system is unavailable,
# you can adjust the REQUIRED flag or provide Boost headers via include paths.

# Find RapidJSON (header-only)
find_package(RapidJSON QUIET)
# If not found, assume RapidJSON is provided in project (e.g., lib/rapidjson include path)
if(NOT RapidJSON_FOUND)
    message(STATUS "RapidJSON not found via find_package, assuming it's in project lib.")
    set(RAPIDJSON_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/lib/rapidjson")
endif()

# 3. Source files
# Explicitly list sources for clarity (you can update this when adding files)
set(GMA_CORE_SOURCES
    src/core/AtomicFunctions.cpp
    src/core/AtomicStore.cpp
    src/core/ClientConnection.cpp
    src/core/FunctionMap.cpp
    src/core/JsonValidator.cpp
    src/core/MarketDispatcher.cpp
    src/core/OrderBookManager.cpp
    src/core/RequestRegistry.cpp
    src/core/ThreadPool.cpp
    src/core/TreeBuilder.cpp
    src/nodes/Aggregate.cpp
    src/nodes/AtomicAccessor.cpp
    src/nodes/Interval.cpp
    src/nodes/Listener.cpp
    src/nodes/Responder.cpp
    src/nodes/SymbolSplit.cpp
    src/nodes/Worker.cpp
    src/ob/ObEngine.cpp
    src/ob/ObKey.cpp
    src/ob/ObMaterializer.cpp
    src/ob/ObProvider.cpp
    # ... (add other core .cpp files here)
    src/ta/TAComputer.cpp
    src/util/Config.cpp
    src/util/Logger.cpp
    src/util/Metrics.cpp
)
set(GMA_SERVER_SOURCES
    src/server/ClientSession.cpp
    src/server/FeedServer.cpp
    src/server/WebSocketServer.cpp
    src/ws/ClientConnection.cpp
    src/ws/RequestRegistry.cpp
    src/ws/WSResponder.cpp
    src/ws/WsBridge.cpp
)
# Combine sources based on BUILD_SERVER option
if(BUILD_SERVER)
    list(APPEND GMA_CORE_SOURCES ${GMA_SERVER_SOURCES})
endif()

# 4. Define Library Target
add_library(gma STATIC)
target_sources(gma PRIVATE ${GMA_CORE_SOURCES})
target_include_directories(gma
    PUBLIC 
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        ${PROJECT_SOURCE_DIR}/include  # ensure headers visible for this build
        ${RAPIDJSON_INCLUDE_DIR}
    PRIVATE 
        ${PROJECT_SOURCE_DIR}/src  # if any source-specific includes
)
# If Boost was found, use its include dirs; else, use vendored headers
if(Boost_FOUND)
    target_include_directories(gma PUBLIC ${Boost_INCLUDE_DIRS})
else()
    target_include_directories(gma PUBLIC ${PROJECT_SOURCE_DIR}/lib/boost)
endif()

# Link required libraries
target_link_libraries(gma 
    PUBLIC 
        Threads::Threads 
        # Link Boost.System if available (Beast/Asio need it for error_code)
        $<$<BOOL:${Boost_SYSTEM_LIBRARY}>:${Boost_SYSTEM_LIBRARY}>
)
# Note: The generator expression above links Boost::system if it’s found as a library.
# If header-only mode, Boost_SYSTEM_LIBRARY might be empty, and this does nothing.

# 5. Define Executable (main program)
if(BUILD_SERVER)
    add_executable(gma_main src/main.cpp)
    target_link_libraries(gma_main PRIVATE gma)
    # You could also embed some version info or link other libs as needed.
endif()

# 6. Tests (if enabled, add subdirectory)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
