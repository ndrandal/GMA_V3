# `gma_data` — design sketch, NOT in the build

Header sketches for the data layer designed in [`../docs/data-layer.md`](../docs/data-layer.md)
(ENC-1478). **Nothing here is compiled.** There is no `add_library(gma_data …)` in
`CMakeLists.txt` and Arrow is not vendored, by design — these are declarations for review,
not an implementation.

So that they can be *read* without an Arrow install, Arrow types appear only as forward
declarations (`arrow::RecordBatch`, `arrow::Schema`) behind `shared_ptr`. The real headers
will include `<arrow/api.h>`.

```
include/gma/data/
  Schema.hpp           canonical Arrow schemas + the POD records the engine sees
  IProviderAdapter.hpp provider adapter + normalizer
  ISource.hpp          ISource, LiveSource, ReplaySource, SplicedSource
  IHistoryBackend.hpp  history backend + Parquet/DuckDB and ClickHouse stubs
  ISinkNode.hpp        sink node (gma -> history), and RecordingSink
schema/
  clickhouse.sql       DDL matching Schema.hpp
```

Layering, which is the one thing not to get wrong: `gma_data` depends on `gma_engine`.
`gma_engine` depends on **neither** `gma_data` nor any connector — Arrow must never
become an engine dependency (`docs/data-layer.md` C9).
