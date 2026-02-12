# tests/CMakeLists.txt
#
# Alternative per-module test build (not used by default — root CMakeLists
# uses monolithic GLOB_RECURSE approach instead).
# To use this, add_subdirectory(tests) from the root and remove the GLOB_RECURSE block.

enable_testing()

# List of test subdirectories — one CMakeLists per module
set(TEST_MODULES
  book
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

foreach(mod IN LISTS TEST_MODULES)
  add_subdirectory(${mod})
endforeach()
