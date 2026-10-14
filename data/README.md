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
SPEC D14). That is a shipping constraint, not a preference. Read D14 as it is written,
though: the client profile is **"live-only *or* reads a central L1 over the wire with no
local store"** — two variants, not one. The **live-only** variant omits this library
entirely, so `cmake -DGMA_BUILD_DATA=OFF` must build and link `gma_server` with no Arrow
anywhere; it does, and that is checked. The **over-the-wire** variant needs Arrow IPC to
read `FORMAT ArrowStream`, so it *does* ship `gma_data` — "the client profile omits this
library" is true of one variant and not of D14. The invariant is unaffected either way,
because it constrains **`gma_engine`**, not the product: `gma_data` is linked into a
deployment or it is not, and `gma_engine` must build in both cases.

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
dirs are `include/` or a vendored `third_party/` tree, and no data-layer library anywhere in
`gma_engine`'s or `gma_server`'s **transitive** link closure — refuses to report a pass on
vacuous input, and ships a `--selftest` that runs each predicate against an input that
violates it. Every way of breaking this invariant otherwise still compiles and still passes
every existing test.

The include-path check is the **weakest of the four, deliberately** (ENC-1497). It used to
read "any in-repo include dir that is not `include/`", which is an over-approximation:
`/third_party/` is gitignored, so it is populated in the shared checkout and absent in a
fresh `git worktree`, and CMake's `find_path` resolves RapidJSON to
`third_party/rapidjson` in the first and `/usr/include` in the second — **the same commit
failed the gate from one checkout and passed from the other**, with nothing in either run's
output to explain the difference. A vendored third-party include tree is not Arrow, and
rapidjson is a long-standing engine dependency (`gma::Event` holds a
`shared_ptr<rapidjson::Document>`), so both resolutions are legal. The exemption is bounded
— only under `third_party/`, decided on `realpath`, and never for a path naming arrow /
parquet / gandiva / clickhouse / duckdb / `gma_data` — and the resolved rapidjson path is
now in the manifest and in the gate's summary, because a verdict that depends on the
checkout has to print the thing that differs. `data/include` and both connector include
roots still fail: that is what this check is for. The *invariant* is carried by the include
scan and the two link-closure checks, not by this one.

The last of the four is transitive because the direct property is the wrong place to look:
`gma_server` links the `gma` umbrella and nothing else, so one
`target_link_libraries(gma INTERFACE gma_data)` would make `GMA_BUILD_DATA=OFF` unlinkable
while `gma_engine`'s and `gma_server`'s own link libraries stayed spotless. CMake emits one
`link.<target>` row per first-party target and the checker walks the graph. `gma_data`'s own
row is the gate's **positive control**: its closure must trip a forbidden pattern (it links
`gma::arrow`), or the patterns match nothing in this build and a clean verdict means nothing.

## What is real and what is still a sketch

| file | state |
|---|---|
| `include/gma/data/Schema.hpp` + `src/Schema.cpp` | **real** — the three first-slice Arrow schemas, `quotes`, and the column-wise codec |
| `../include/gma/data/Record.hpp` | **real** — the engine-facing PODs (in `gma_engine`) |
| `../include/gma/data/SymbolTable.hpp` | **real** — ticker/venue interning (in `gma_engine`) |
| `include/gma/data/DatabentoAdapter.hpp` + `src/DatabentoAdapter.cpp` | **real** — the DBN decoder and the DBN→canonical mapping (ENC-1482) |
| `tests/fixtures/dbn/` | **real Databento DBN bytes**, Apache-2.0 — see `PROVENANCE.md` there |
| `include/gma/data/IProviderAdapter.hpp` | sketch — the `IProviderAdapter` / `INormalizer` interfaces. `DatabentoAdapter` deliberately does **not** implement it yet: that interface carries `subscribeFrame` / `snapshotRequestFrame` / `fetchHistory`, which are live-gateway and HTTP concerns outside ENC-1482's scope |
| `include/gma/data/Shim.hpp` | sketch — ENC-1485 and the post-measurement plan |
| `include/gma/data/IHistoryBackend.hpp` | sketch — `IL1Store`, ClickHouse + fixture impls |
| `include/gma/data/ISource.hpp` | sketch — `ISource`, `LiveSource`, `ReplaySource`, `SplicedSource` |
| `include/gma/data/ISinkNode.hpp` | sketch — research output, parity recording |
| `schema/clickhouse.sql` | the L1 DDL — ENC-1480 |

The sketches use forward-declared Arrow types (`arrow::RecordBatch`, `arrow::Schema`) behind
`shared_ptr` so they read without an Arrow install. `Schema.hpp` no longer needs to: it is
compiled.

## The Databento adapter in one paragraph

`DatabentoAdapter` turns DBN bytes into `TickRecord`/`BookRecord` vectors and, through
`Schema.hpp`, into canonical Arrow batches. It decodes and maps; it computes nothing. **Q14 is
resolved in favour of decoding DBN ourselves** rather than linking `databento-cpp` — which
needs OpenSSL, zstd, nlohmann_json, cpp-httplib, HowardHinnant/date *and a Rust toolchain*
(it delegates DBN decoding to the Rust `dbn` crate's C-FFI) for a 90%-unused HTTP client;
the full trade-off, stated in both directions, is `../docs/data-layer.md` §15.2. zstd is
deliberately not a dependency: `.dbn.zst` is decompressed by the ingest job, and feeding a
zstd frame here is refused by name. The mapping is a re-encoding of `ItchAdapter`'s tested
MBO vocabulary, and the three invariants it preserves are the book-neutral print, the epoch
bump on a clear (whose mechanism here is the Clear case's **erase loop**, not an epoch in the
adapter's own order key — that key has two members), and `Cancel` being a **delta** that needs
the resting size. One thing it does *not* inherit: `bookNeutral` is not a repo-wide invariant
(`FeedEvent.hpp` defaults it false and `WsFeedClient` has a live branch that consumes the
book), so "every `BookAction::Trade` row is book-neutral" is **this adapter's** decision,
justified by DBN's own contract, and a future non-DBN provider owes its own answer. The DBN
layout is established by five committed files of **real vendor bytes** (`tests/fixtures/dbn/`,
Apache-2.0) rather than by assertion — the test's first act is to prove a DBN encoder
reproduces each of them byte for byte, and only then uses it to reach the actions a
two-record sample cannot.

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

**`GMA_BUILD_DATA` defaults to `GMA_BUILD_TESTS`.** With tests on, `gma_data`,
`gma_data_tests` and both engine-isolation cases are all built and run — nothing is
silently skipped where tests run, which a fixed default could not give you in both
directions. With tests off, a bare Release configure fetches **no** Arrow and still builds
`gma_server`, so the quickstart works offline. An explicit `-DGMA_BUILD_DATA=ON|OFF` always
wins, and the configure prints which way it resolved and why.

```bash
# Tests on => gma_data on by default (Arrow is found or fetched).
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure

# The quickstart: tests off => gma_data off by default => no Arrow download.
cmake -B build-release -DCMAKE_BUILD_TYPE=Release

# SPEC D14's live-only client variant, with tests: no local store, no Arrow at all.
cmake -B build-liveonly -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON -DGMA_BUILD_DATA=OFF

# The data layer without the engine test suite.
cmake -B build-dataonly -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_DATA=ON

# Point the Arrow fallback at an already-downloaded archive instead of the network.
cmake -B build -DGMA_DATA_ARROW_URL=/path/to/apache-arrow-23.0.1.tar.gz

# Ignore an installed-but-unusable Arrow and build it from source regardless.
cmake -B build -DGMA_BUILD_DATA=ON -DGMA_DATA_ARROW_FORCE_FETCH=ON
```

Arrow's measured cost on this toolchain is in `docs/data-layer.md` §15 — it is far cheaper
than the estimate that section was written with.
