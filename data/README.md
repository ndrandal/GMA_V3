# `gma_data` — design sketch, NOT in the build

Header sketches for the data and query layer designed in
[`../docs/data-layer.md`](../docs/data-layer.md) (ENC-1478). **Nothing here is compiled.**
There is no `add_library(gma_data …)` in `CMakeLists.txt` and Arrow is not vendored, by
design — these are declarations for review, not an implementation.

So that they can be *read* without an Arrow install, Arrow types appear only as forward
declarations (`arrow::RecordBatch`, `arrow::Schema`) behind `shared_ptr`.

## The one thing to understand before reading any of it

> **ClickHouse does selection. gma does computation. Nothing computes a value twice.**

Not architectural taste — a product requirement. A transpiler that reimplemented gma's math
in SQL would differ in float accumulation order, bucket edge inclusivity, EMA seeding and
gap handling, and this system exists to read differences of that size as *signal*. A
discrepancy manufactured by the storage layer is indistinguishable from an indication, so
there is no tolerance at which a second implementation is safe. See `docs/data-layer.md`
§1.

Consequences visible in these headers: `IL1Store` has no `interval` parameter and cannot
aggregate; the shim's `Planner` analyses the graph's *data dependencies* and never its
math; and the boundary is enforced by `SqlLint` at build time rather than by a parity diff.

```
include/gma/data/
  Schema.hpp           canonical L1 schema + the PODs the engine sees
  IProviderAdapter.hpp provider adapter (Databento DBN) + normalizer
  Shim.hpp             THE CENTERPIECE — planner, SQL lint, transport, decode
  IHistoryBackend.hpp  IL1Store (selection only) + ClickHouse and fixture impls
  ISource.hpp          ISource, LiveSource, ReplaySource, SplicedSource
  ISinkNode.hpp        SinkNode (research output), RecordingSink (parity)
schema/
  clickhouse.sql       L1 DDL — ReplacingMergeTree, projections, completeness
```

Layering, which is the one thing not to get wrong: `gma_data` depends on `gma_engine`.
`gma_engine` depends on **neither** `gma_data` nor any connector — Arrow and the ClickHouse
client must never become engine dependencies (`docs/data-layer.md` §4 rule 6).

Parquet and DuckDB are **not** here. They were the previous revision's default backend and
were dropped when ClickHouse L1 became the store; DuckDB was the largest single dependency
in the design and existed only to query Parquet (`docs/data-layer.md` C10, §15).
