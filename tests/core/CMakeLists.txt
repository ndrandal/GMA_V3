# tests/core/CMakeLists.txt
add_executable(tests_core
    ConfigTest.cpp
    LoggerTest.cpp
    ThreadPoolTest.cpp
    AtomicStoreTest.cpp
    AtomicFunctionsTest.cpp
)

target_link_libraries(tests_core
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_core COMMAND tests_core)
