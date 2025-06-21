# tests/treebuilder/CMakeLists.txt
add_executable(tests_treebuilder
    TreeBuilderTest.cpp
)

target_link_libraries(tests_treebuilder
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_treebuilder COMMAND tests_treebuilder)
