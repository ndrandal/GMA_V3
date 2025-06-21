# tests/CMakeLists.txt
#
# This is included by the top‑level CMakeLists, after
#   set(GTEST_CTEST_RUNNER cmd)
# to wire up all the test subfolders.

# Make sure tests are enabled
enable_testing()

# Force GoogleTest discovery to use cmd.exe (not pwsh)
# This must be a CACHE variable so it is seen in all subdirectories.
set(GTEST_CTEST_RUNNER cmd CACHE STRING "Use cmd.exe for GTest discovery" FORCE)

# List of test subdirectories — one CMakeLists per module
set(TEST_MODULES
  connection
  context
  core
  dispatch
  integration
  mapping
  nodes
  registry
  treebuilder
  validation
)

# Add each module
foreach(mod IN LISTS TEST_MODULES)
  add_subdirectory(${mod})
endforeach()
