# tests/mapping/CMakeLists.txt
add_executable(tests_mapping
    FunctionMapTest.cpp
)

target_link_libraries(tests_mapping
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_mapping COMMAND tests_mapping)
