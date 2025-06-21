# tests/context/CMakeLists.txt
add_executable(tests_context
    ContextTest.cpp
)

target_link_libraries(tests_context
    gma
    GTest::gtest_main
)

enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_context COMMAND tests_context)

