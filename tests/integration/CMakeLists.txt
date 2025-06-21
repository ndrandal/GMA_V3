# tests/integration/CMakeLists.txt

add_executable(tests_integration
  IntegrationTest.cpp
  StressTest.cpp
)

# Link only to your library and GoogleTest
target_link_libraries(tests_integration
  PRIVATE
    gma
    GTest::gtest_main
)

# Bring in the GoogleTest helper macros
enable_testing()               # once per CMakeLists (can be in tests/CMakeLists.txt)
add_test(NAME tests_integration COMMAND tests_integration)
