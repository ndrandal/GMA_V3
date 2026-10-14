# ===========================================================================
# gma_data — L1 schema, codec and (later) shim (ENC-1481)
#
# Design: specs/2026-10-14-gma-data-layer/SPEC.md; docs/data-layer.md.
#
# LAYERING, WHICH IS THE ONE THING NOT TO GET WRONG:
#
#     gma_data  ->  gma_engine          (this direction only)
#
# `gma_engine` depends on neither this library nor any connector, and ARROW
# AND THE CLICKHOUSE CLIENT MUST NEVER BECOME ENGINE DEPENDENCIES (SPEC D14,
# docs/data-layer.md §4 rule 6). That is not style: SPEC D14's client
# deployment profile is live-only and omits this library entirely, so
# `gma_server` must link and run with GMA_BUILD_DATA=OFF and nothing missing.
#
# The invariant is asserted, not asserted-about: `tools/check_engine_isolation.py`
# runs as the ctest case `gma_engine_isolation` and fails the build if an Arrow
# or ClickHouse include appears in engine code, if a `data/` header becomes
# reachable from the engine's include path, or if `gma_data` enters
# `gma_engine`'s link libraries.
#
# The engine-facing PODs are deliberately NOT here — they live in
# `include/gma/data/Record.hpp`, inside gma_engine, because ENC-1486's
# `Dispatcher::onRecord` is engine code and has to name the type. The split is
# explained at the top of that header.
# ===========================================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GmaDataArrow)

add_library(gma_data STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Schema.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/gma/data/Schema.hpp"
)

# PUBLIC: a consumer of gma_data sees the L1 schema headers.
target_include_directories(gma_data PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

# gma_engine, and not `gma`: this library needs the engine's PODs and
# SymbolTable, and must not acquire the market connector by accident.
target_link_libraries(gma_data PUBLIC gma_engine gma::arrow)

gma_apply_warnings(gma_data)

message(STATUS "gma_data: enabled (Arrow: ${GMA_DATA_ARROW_PROVENANCE})")

# ---------------------------------------------------------------------------
# Tests — a SEPARATE binary from gma_tests, on purpose.
# ---------------------------------------------------------------------------
# Linking gma_data into `gma_tests` would put Arrow in the same executable as
# every engine test, so a stray `#include <arrow/...>` in engine test code
# would compile. Keeping the binaries apart means the engine test suite is
# still built against an Arrow-free include path, which is the thing being
# protected.
if (GMA_BUILD_TESTS AND TARGET GTest::gtest_main)
  file(GLOB GMA_DATA_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  add_executable(gma_data_tests ${GMA_DATA_TEST_SOURCES})
  target_link_libraries(gma_data_tests PRIVATE gma_data GTest::gtest_main)
  gma_apply_warnings(gma_data_tests)
  add_test(NAME gma_data_tests
    COMMAND gma_data_tests
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:gma_data_tests>")
endif()
