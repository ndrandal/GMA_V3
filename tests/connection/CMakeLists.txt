# tests/connection/CMakeLists.txt
add_executable(tests_connection
    ConnectionTest.cpp
)

target_link_libraries(tests_connection
    gma
    GTest::gtest_main
)

enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_connection COMMAND tests_connection)
