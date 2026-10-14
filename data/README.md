# `gma_data` — the L1 data and query layer

The library designed in [`../docs/data-layer.md`](../docs/data-layer.md) (ENC-1478) and
locked in [`../specs/2026-10-14-gma-data-layer/SPEC.md`](../specs/2026-10-14-gma-data-layer/SPEC.md).

**As of ENC-1481 the schema half is real and in the build.** `add_library(gma_data …)`
lives in [`CMakeLists.txt`](CMakeLists.txt), Arrow is found-or-fetched by
[`cmake/GmaDataArrow.cmake`](cmake/GmaDataArrow.cmake), and `ctest` runs
`gma_data_tests`. Everything else in `include/gma/data/` is still a **declaration for
review, not an implementation** — see *What is real and what is still a sketch* below.

## The one thing to understand before reading any of it

> **ClickHouse does selection. gma does computation. Nothing computes a value twice.**

Not architectural taste — a product requirement. A transpiler that reimplemented gma's math
in SQL would differ in float accumulation order, bucket edge inclusivity, EMA seeding and
gap handling, and this system exists to read differences of that size as *signal*. A
discrepancy manufactured by the storage layer is indistinguishable from an indication, so
there is no tolerance at which a second implementation is safe. See `docs/data-layer.md`
§1 and SPEC D1.

Consequences visible in these headers: `IL1Store` has no `interval` parameter and cannot
aggregate; the shim's `Planner` analyses the graph's *data dependencies* and never its
math; and the boundary is enforced by `SqlLint` at build time rather than by a parity diff.
It is also why `ARROW_COMPUTE` and `ARROW_ACERO` are off in `cmake/GmaDataArrow.cmake` and
must stay off: an Arrow kernel evaluating an expression is a second implementation of an
operator, which D1 forbids outright.

## Layering — the thing not to get wrong

```
  gma_data  ───▶  gma_engine          (this direction only)
```

`gma_engine` depends on **neither** `gma_data` nor any connector, and **Arrow and the
ClickHouse client must never become engine dependencies** (`docs/data-layer.md` §4 rule 6,
SPEC D14). That is a shipping constraint, not a preference: SPEC D14's **client deployment
profile is live-only** and omits this library entirely, so `cmake -DGMA_BUILD_DATA=OFF`
must build and link `gma_server` with no Arrow anywhere. It does, and that is checked.

**The engine-facing PODs are deliberately NOT in this directory.** `RecordHeader`,
`TickRecord`, `BookRecord` and `SnapshotRecord` live in
[`../include/gma/data/Record.hpp`](../include/gma/data/Record.hpp), inside `gma_engine`,
because ENC-1486 adds `Dispatcher::onRecord(const data::TickRecord&)` and `Dispatcher` *is*
engine code — the engine has to be able to name a record. They are Arrow-free and
dependency-free; the `arrow::Schema`s and the codec, which are not, stay here. One
namespace (`gma::data`), one definition of each type, two libraries. `SymbolTable` sits
beside the PODs for the same reason: interning happens here in decode, but resolving an id
back to a ticker happens at an output boundary, and every output boundary is engine code.

So the invariant worth encoding is **not** "no `gma/data` header is reachable from engine
code" but:

> `gma_engine` must not acquire a dependency on Arrow, the ClickHouse client, or the
> `gma_data` library target.

[`../tools/check_engine_isolation.py`](../tools/check_engine_isolation.py) asserts exactly
that, as the ctest cases `gma_engine_isolation` and `gma_engine_isolation_selftest`. It
checks four things — no forbidden include in engine code or in the composition root, no
`data/`-only or connector header reached from the engine, `gma_engine`'s in-repo include
path is `include/` only, and neither `gma_engine` nor `gma_server` links a data-layer
library — refuses to report a pass on vacuous input, and ships a `--selftest` that runs
each predicate against an input that violates it. Every way of breaking this invariant
otherwise still compiles and still passes every existing test.

## What is real and what is still a sketch

| file | state |
|---|---|
| `include/gma/data/Schema.hpp` + `src/Schema.cpp` | **real** — the three first-slice Arrow schemas, `quotes`, and the column-wise codec |
| `../include/gma/data/Record.hpp` | **real** — the engine-facing PODs (in `gma_engine`) |
| `../include/gma/data/SymbolTable.hpp` | **real** — ticker/venue interning (in `gma_engine`) |
| `include/gma/data/IProviderAdapter.hpp` | sketch — ENC-1482 (Databento DBN) |
| `include/gma/data/Shim.hpp` | sketch — ENC-1485 and the post-measurement plan |
| `include/gma/data/IHistoryBackend.hpp` | sketch — `IL1Store`, ClickHouse + fixture impls |
| `include/gma/data/ISource.hpp` | sketch — `ISource`, `LiveSource`, `ReplaySource`, `SplicedSource` |
| `include/gma/data/ISinkNode.hpp` | sketch — research output, parity recording |
| `schema/clickhouse.sql` | the L1 DDL — ENC-1480 |

The sketches use forward-declared Arrow types (`arrow::RecordBatch`, `arrow::Schema`) behind
`shared_ptr` so they read without an Arrow install. `Schema.hpp` no longer needs to: it is
compiled.

## The schema in one paragraph

Three tables, not one union table, because a unified event table needs every type's fields
nullable — which defeats the codecs and makes every read scan columns it does not want
(§8.4). Every table carries the same seven **common columns** first, all non-nullable:
`ts_event` and `ts_recv` as nanosecond UTC timestamps (`ts_recv` is **diagnostics only** and
never an operator input), a non-null `provider_seq`, a `session_id`, `symbol` as the ticker
in a `dictionary<int32, utf8>`, `instrument_id` for DBN provenance, and `venue` in a
`dictionary<int16, utf8>`. **Prices are `Int64` at a fixed global 1e-9 scale** (SPEC D8) —
no per-row scale column, no per-symbol lookup on decode, and an exact integer path from DBN
with no double round trip. `OrderBookManager`'s per-symbol tick size stays a
book-quantization concern and does not reach the schema.

Parquet and DuckDB are **not** here and are not coming. They were the previous revision's
default backend and were dropped entirely when ClickHouse L1 became the store; DuckDB was
the largest single dependency in the design and existed only to query Parquet (SPEC D14,
`docs/data-layer.md` C10, §15).

## Build

```bash
# Default: gma_data is built (Arrow is found or fetched).
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# SPEC D14's live-only client profile: no local store, no Arrow fetched at all.
cmake -B build-liveonly -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON -DGMA_BUILD_DATA=OFF

# Point the Arrow fallback at an already-downloaded archive instead of the network.
cmake -B build -DGMA_DATA_ARROW_URL=/path/to/apache-arrow-23.0.1.tar.gz
```

Arrow's measured cost on this toolchain is in `docs/data-layer.md` §15 — it is far cheaper
than the estimate that section was written with.
