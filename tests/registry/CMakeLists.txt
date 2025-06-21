# tests/registry/CMakeLists.txt
add_executable(tests_registry
    RegistryTest.cpp
)

target_link_libraries(tests_registry
    gma
    GTest::gtest_main
)
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_registry COMMAND tests_registry)
