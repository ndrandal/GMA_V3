# ===========================================================================
# Arrow for gma_data (ENC-1481)
#
# Design: specs/2026-10-14-gma-data-layer/SPEC.md D14; docs/data-layer.md §15.
#
# Produces ONE target, `gma::arrow`, and nothing else. Everything downstream
# links that, so whether Arrow came from the system or from source is invisible
# past this file.
#
# TWO ROUTES, IN THIS ORDER
#
#   1. find_package(Arrow CONFIG) — an installed Arrow. Kept working because
#      a machine that has one should not pay for a source build. There is no
#      installed Arrow on the box this was written on, and no way to get one
#      there: `pkg-config --modversion arrow` fails and, while Fedora 44 DOES
#      package it (`libarrow-devel` 23.0.1 — the ticket's claim that dnf has
#      only the unrelated `arrows` package is wrong), installing it needs a
#      sudo password this environment does not have. So route 1 is untested
#      here and route 2 is what the measured number in SPEC §5 describes.
#
#   2. FetchContent of the Apache source release, trimmed hard.
#
# WHAT IS OFF AND WHY IT IS NOT A SIZE TWEAK
#
# `ARROW_PARQUET=OFF` and the absence of DuckDB are SPEC D14 — Parquet and
# DuckDB were DROPPED ENTIRELY from this design when ClickHouse became L1, and
# DuckDB was the largest single dependency in the previous revision. Turning
# Parquet back on is a design change, not a build tweak.
#
# What gma_data actually needs is small and worth naming precisely:
#   * arrow core — Buffer, Array, Schema, RecordBatch, the builders.
#   * arrow IPC  — because ENC-1485 measures ClickHouse `FORMAT ArrowStream`
#                  over HTTP, which is Arrow IPC on the wire.
# Nothing else. Compute/Acero are off and must stay off: a kernel evaluated in
# Arrow is a second implementation of an operator, which SPEC D1 forbids
# outright. That is the strongest reason on this list and the one most likely
# to be "helpfully" reverted.
#
# `ARROW_DEFINE_OPTIONS` is forced ON. As a subproject Arrow defaults it OFF,
# which leaves every ARROW_* variable undefined rather than defaulted — an
# undefined option reads as false, so the build would mostly work, and
# `ARROW_DEPENDENCY_SOURCE` / `ARROW_SIMD_LEVEL` would be empty strings instead
# of valid values. With it ON, Arrow defines its own options with real
# defaults and the explicit cache entries below win over them, so the trim is
# visible in CMakeCache.txt and auditable.
# ===========================================================================

if (TARGET gma::arrow)
  return()
endif()

set(GMA_DATA_ARROW_VERSION "23.0.1" CACHE STRING
  "Apache Arrow version fetched when no installed Arrow is found")
set(GMA_DATA_ARROW_URL
  "https://archive.apache.org/dist/arrow/arrow-${GMA_DATA_ARROW_VERSION}/apache-arrow-${GMA_DATA_ARROW_VERSION}.tar.gz"
  CACHE STRING
  "Source archive for the Arrow fallback build. May be a local file:// path or an absolute path to a already-downloaded tarball.")
set(GMA_DATA_ARROW_SHA256
  "bd09adb4feac11fe49d1604f296618866702be610c86e2d513b561d877de6b18"
  CACHE STRING
  "SHA256 of GMA_DATA_ARROW_URL. Set to the empty string to skip the check when pointing at a different archive.")

option(GMA_DATA_ARROW_IPC_CODECS
  "Build Arrow's LZ4 and ZSTD codecs. ON because ClickHouse's ArrowStream output compresses with lz4_frame by DEFAULT (output_format_arrow_compression_method), so a codec-free Arrow cannot read ENC-1485's stream at all without a per-query SETTINGS override."
  ON)

# ---------------------------------------------------------------------------
# Route 1 — an installed Arrow.
# ---------------------------------------------------------------------------
find_package(Arrow CONFIG QUIET)

if (Arrow_FOUND)
  set(_gma_arrow_impl "")
  foreach (_cand Arrow::arrow_static Arrow::arrow_shared arrow_static arrow_shared)
    if (TARGET ${_cand})
      set(_gma_arrow_impl ${_cand})
      break()
    endif()
  endforeach()
  if (_gma_arrow_impl STREQUAL "")
    message(FATAL_ERROR
      "find_package(Arrow) succeeded but defined none of "
      "Arrow::arrow_static / Arrow::arrow_shared / arrow_static / arrow_shared. "
      "Refusing to guess a target name — set -DGMA_DATA_ARROW_FORCE_FETCH=ON to "
      "build Arrow from source instead.")
  endif()
  # GLOBAL: an IMPORTED target is directory-scoped by default, and this module is
# included from data/, so without it `gma::arrow` would be invisible to any
# consumer defined at the root — including a future benchmark target.
add_library(gma::arrow INTERFACE IMPORTED GLOBAL)
  target_link_libraries(gma::arrow INTERFACE ${_gma_arrow_impl})
  set(GMA_DATA_ARROW_PROVENANCE "system (find_package, ${ARROW_VERSION}, target ${_gma_arrow_impl})"
      CACHE INTERNAL "" FORCE)
  message(STATUS "gma_data: Arrow ${ARROW_VERSION} found via find_package -> ${_gma_arrow_impl}")
  return()
endif()

# ---------------------------------------------------------------------------
# Route 2 — build it from source, trimmed.
# ---------------------------------------------------------------------------
message(STATUS
  "gma_data: no installed Arrow (find_package(Arrow CONFIG) failed) — "
  "building ${GMA_DATA_ARROW_VERSION} from source. "
  "This is the long pole in a cold configure; see docs/data-layer.md §15.")

include(FetchContent)

# Arrow as a subproject: define its options so our overrides below are the ones
# that win, rather than silently landing on an undefined variable.
set(ARROW_DEFINE_OPTIONS ON CACHE BOOL "" FORCE)

# --- What we need -----------------------------------------------------------
set(ARROW_IPC                       ON  CACHE BOOL "" FORCE)  # ENC-1485: FORMAT ArrowStream
set(ARROW_BUILD_STATIC              ON  CACHE BOOL "" FORCE)
set(ARROW_ENABLE_THREADING          ON  CACHE BOOL "" FORCE)
set(ARROW_POSITION_INDEPENDENT_CODE ON  CACHE BOOL "" FORCE)

# --- What we do not, stated explicitly even where Arrow already defaults it
# off. An explicit OFF is a decision with a reason; a default is an accident
# that a future Arrow release is free to change.
set(ARROW_PARQUET        OFF CACHE BOOL "" FORCE)  # SPEC D14 — DROPPED from the design
set(ARROW_ORC            OFF CACHE BOOL "" FORCE)
set(ARROW_COMPUTE        OFF CACHE BOOL "" FORCE)  # SPEC D1 — a kernel is a second operator
set(ARROW_ACERO          OFF CACHE BOOL "" FORCE)  # SPEC D1
set(ARROW_DATASET        OFF CACHE BOOL "" FORCE)
set(ARROW_SUBSTRAIT      OFF CACHE BOOL "" FORCE)  # SPEC D1 — a query plan is a transpiler
set(ARROW_GANDIVA        OFF CACHE BOOL "" FORCE)  # SPEC D1 — an expression JIT, likewise
set(ARROW_FLIGHT         OFF CACHE BOOL "" FORCE)
set(ARROW_FLIGHT_SQL     OFF CACHE BOOL "" FORCE)
set(ARROW_FLIGHT_SQL_ODBC OFF CACHE BOOL "" FORCE)
set(ARROW_CSV            OFF CACHE BOOL "" FORCE)
set(ARROW_JSON           OFF CACHE BOOL "" FORCE)
set(ARROW_FILESYSTEM     OFF CACHE BOOL "" FORCE)  # L1 is ClickHouse; no local file store
set(ARROW_S3             OFF CACHE BOOL "" FORCE)
set(ARROW_S3_MODULE      OFF CACHE BOOL "" FORCE)
set(ARROW_GCS            OFF CACHE BOOL "" FORCE)
set(ARROW_AZURE          OFF CACHE BOOL "" FORCE)
set(ARROW_HDFS           OFF CACHE BOOL "" FORCE)
set(ARROW_PYTHON         OFF CACHE BOOL "" FORCE)  # removed in modern Arrow; harmless
set(ARROW_TESTING        OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_INTEGRATION OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_UTILITIES OFF CACHE BOOL "" FORCE)
set(ARROW_BUILD_SHARED   OFF CACHE BOOL "" FORCE)  # static works; halves the build
set(ARROW_JEMALLOC       OFF CACHE BOOL "" FORCE)
set(ARROW_MIMALLOC       OFF CACHE BOOL "" FORCE)  # Arrow defaults this ON
set(ARROW_WITH_RE2       OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_UTF8PROC  OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_OPENTELEMETRY OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_UCX       OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_BROTLI    OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_BZ2       OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_SNAPPY    OFF CACHE BOOL "" FORCE)
set(ARROW_WITH_ZLIB      OFF CACHE BOOL "" FORCE)
set(ARROW_USE_CCACHE     OFF CACHE BOOL "" FORCE)

if (GMA_DATA_ARROW_IPC_CODECS)
  set(ARROW_WITH_LZ4  ON CACHE BOOL "" FORCE)
  set(ARROW_WITH_ZSTD ON CACHE BOOL "" FORCE)
else()
  set(ARROW_WITH_LZ4  OFF CACHE BOOL "" FORCE)
  set(ARROW_WITH_ZSTD OFF CACHE BOOL "" FORCE)
endif()

# Arrow is C++17 and sets CMAKE_CXX_STANDARD while configuring. Restore ours
# afterwards: gma is C++20 and `include/gma/data/Record.hpp` uses C++20 in
# engine code that is compiled long after this file runs.
set(_gma_saved_cxx_standard "${CMAKE_CXX_STANDARD}")

set(_gma_arrow_hash "")
if (NOT GMA_DATA_ARROW_SHA256 STREQUAL "")
  set(_gma_arrow_hash URL_HASH "SHA256=${GMA_DATA_ARROW_SHA256}")
endif()

FetchContent_Declare(
  arrow
  URL           "${GMA_DATA_ARROW_URL}"
  ${_gma_arrow_hash}
  # The release archive is the whole Arrow monorepo; the C++ build is one
  # subdirectory of it. Without this, CMake adds the top level, which has no
  # CMakeLists.txt at all.
  SOURCE_SUBDIR cpp
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(arrow)

set(CMAKE_CXX_STANDARD "${_gma_saved_cxx_standard}")

if (NOT TARGET arrow_static)
  message(FATAL_ERROR
    "Arrow ${GMA_DATA_ARROW_VERSION} was fetched but defined no `arrow_static` "
    "target. Arrow renames its in-tree targets between releases; check "
    "cpp/cmake_modules/BuildUtils.cmake in the fetched tree "
    "(${arrow_SOURCE_DIR}) and update this file.")
endif()

# GLOBAL: an IMPORTED target is directory-scoped by default, and this module is
# included from data/, so without it `gma::arrow` would be invisible to any
# consumer defined at the root — including a future benchmark target.
add_library(gma::arrow INTERFACE IMPORTED GLOBAL)
target_link_libraries(gma::arrow INTERFACE arrow_static)
# Arrow's in-tree static target does not carry the generated-header dir for
# consumers in every release; add both explicitly so a #include <arrow/api.h>
# resolves regardless.
target_include_directories(gma::arrow SYSTEM INTERFACE
  "${arrow_SOURCE_DIR}/cpp/src"
  "${arrow_BINARY_DIR}/src")

set(GMA_DATA_ARROW_PROVENANCE "source (FetchContent ${GMA_DATA_ARROW_VERSION})"
    CACHE INTERNAL "" FORCE)
