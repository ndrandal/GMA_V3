# tests/validation/CMakeLists.txt

add_executable(tests_validation
    ValidationTest.cpp
)

target_link_libraries(tests_validation
    PRIVATE
      gma
      GTest::gtest_main
)

enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_validation COMMAND tests_validation)
