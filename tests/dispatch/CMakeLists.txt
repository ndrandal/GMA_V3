# tests/dispatch/CMakeLists.txt
add_executable(tests_dispatch
    DispatchTest.cpp
)

target_link_libraries(tests_dispatch
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_dispatch COMMAND tests_dispatch)
