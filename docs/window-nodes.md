# Window nodes

Two single-purpose nodes for expressing "reduce a stream over a
wall-clock period":

- **`TumblingWindow(periodMs)`** — per-symbol scalar accumulator that
  emits a `vector<double>` at every wall-clock-aligned boundary.
- **`VectorReducer(fn)`** — takes one `vector<double>`, applies a
  reducer from the shared `FunctionMap`, emits a scalar.

Together they express "the max of every NEXO high price observed in a
60-second window" or any other per-period reduction. The OHLC worked
example below shows how four of these expressions feed a compound
buffer to drive per-minute candles.

The two nodes were proposed and landed under
`specs/2026-05-12-gma-window-nodes` (Phase 1 = TumblingWindow,
Phase 2 = VectorReducer + composition + this doc).

## TumblingWindow

A per-symbol scalar accumulator with wall-clock-aligned tumbling-window
emit semantics. For each `(streamKey)` it sees on `onValue`, the node
pushes the incoming scalar into a per-symbol buffer; on every wall-clock
boundary aligned to `periodMs` it emits one
`StreamValue{symbol, std::vector<double>}` downstream and clears the
buffer. Empty buckets are not emitted. Alignment is inherited from
`BucketTime::nextAlignedAfter` so two `TumblingWindow` instances with
the same `periodMs` produce coincident boundaries regardless of when
each was constructed — independent consumers see the same bar edges.

### JSON shape (pipeline stage)

`TumblingWindow` is a pipeline stage, not a tree-shaped node — it composes
via the standard `pipeline:[…]` array in `TreeBuilder` (each stage's
downstream is wired by the reverse-iteration in `buildForRequest`). It
takes no `child` / `input` key.

```json
{
  "streamKey": "NEXO",
  "field": "highPrice",
  "pipeline": [
    { "type": "TumblingWindow", "periodMs": 60000 },
    { "type": "VectorReducer", "fn": "max" }
  ]
}
```

Keys:
- **`periodMs`** (required, integer, > 0, ≤ 3,600,000) — boundary period
  in milliseconds. `ms` is accepted as an alias.

### Emit shape

Each non-empty bucket produces one `StreamValue` whose `symbol` matches
the upstream scalar's `streamKey` and whose `value` holds the variant
alternative `std::vector<double>` containing the scalars accumulated
during the closed window, in arrival order.

### Empty-bucket semantics

If no upstream values arrived during a period, no emit fires for that
boundary. Consumers that need a frame per boundary even on quiet markets
(e.g. a chart that should draw a "no-trade" gap rather than freeze) are
a future `EmitOnEmpty` config addition.

### Lifecycle

Mirrors `BucketTime` and `Interval`:

- Constructed via `std::make_shared<TumblingWindow>(period, downstream, pool)`.
- `start()` must be called after the `shared_ptr` is held. The builder in
  `TreeBuilder::registerBuiltinNodeTypes` calls it automatically.
- `shutdown()` is synchronous: sets a stopping flag, wakes the timer
  thread, joins it (or detaches safely if invoked from the timer thread).
  The downstream pointer and per-symbol buffers are released under the
  internal mutex.

### Resource caps

Per-symbol buffer growth is capped by `MAX_SYMBOLS = 10000` (same
constant as `Worker`). A symbol that would push past the cap is dropped
with a single `WARN`-level log line; existing symbols continue to
accumulate.

## VectorReducer

A thin transform: receive one `StreamValue{symbol, vector<double>}`,
apply a reducer function from the shared `FunctionMap` registry, emit
one `StreamValue{symbol, double}` downstream. Synchronous (no timer
thread, no pool dispatch).

### JSON shape (pipeline stage)

```json
{ "type": "VectorReducer", "fn": "max" }
```

Keys:
- **`fn`** (required, string) — name of a registered reducer in
  `FunctionMap`. Resolved at build time via
  `FunctionMap::instance().getFunction(fn)`; unknown names produce a
  `runtime_error("VectorReducer: unknown fn '<name>'")` which
  `ClientSession`'s subscribe/validate `try { TreeBuilder } catch` chain
  surfaces to the WS peer as
  `{"type":"error","where":"validate","message":...}`.

### Available reducers

`VectorReducer` reuses the same `FunctionMap` registry `Worker` uses, so
every plain reducer that has shipped for `Worker` is usable here. The
built-in set (from `src/core/BuiltinFunctions.cpp`) includes at least:
`sum`, `mean`, `avg`, `max`, `min`, `first`, `last`, `count`, `median`,
`range`, `stddev`, `variance`, `spread`, `midpoint`, `product`.

### Input shape contract

`VectorReducer` is wired to consume `vector<double>` emits — primarily
from `TumblingWindow`. Inputs whose `StreamValue::value` variant holds a
different alternative (a scalar, an int, a string, a `vector<int>`) are
dropped with a single `Warn` line and no downstream emit fires.
Surface-it-loudly rather than reduce a synthetic 1-element vector that
would mask the upstream miswire.

### Lifecycle

- Constructed via `std::make_shared<VectorReducer>(fn, downstream)`.
- No `start()` is required.
- `shutdown()` clears `downstream_` under the internal mutex; further
  `onValue` calls early-return on the stopping flag.

## Composition: per-minute OHLC

Per-minute open / high / low / close candles are four `VectorReducer ←
TumblingWindow` expressions over the same scalar source. Each expression
produces one scalar per minute; the four scalars per minute feed a
compound buffer in embassy (per the saved-chart-slice work in
`specs/2026-05-07-saved-chart-slice`) to drive a candlestick chart.

```json
{
  "streamKey": "NEXO",
  "field": "lastPrice",
  "pipeline": [
    { "type": "TumblingWindow", "periodMs": 60000 },
    { "type": "VectorReducer", "fn": "first" }
  ]
}
```
→ Per-minute **open**: first tick price within each minute.

```json
{ "field": "highPrice",
  "pipeline": [
    { "type": "TumblingWindow", "periodMs": 60000 },
    { "type": "VectorReducer", "fn": "max" }
  ] }
```
→ Per-minute **high**: `max` of every `highPrice` observation in the
minute. (Use `field: "highPrice"` — `MarketTA`'s running OHLC already
maintains a per-tick high.)

```json
{ "field": "lowPrice",
  "pipeline": [
    { "type": "TumblingWindow", "periodMs": 60000 },
    { "type": "VectorReducer", "fn": "min" }
  ] }
```
→ Per-minute **low**: `min` of every `lowPrice` observation.

```json
{ "field": "lastPrice",
  "pipeline": [
    { "type": "TumblingWindow", "periodMs": 60000 },
    { "type": "VectorReducer", "fn": "last" }
  ] }
```
→ Per-minute **close**: last tick price within each minute.

Per-minute volume sum is the same shape with `field: "volume"` and
`fn: "sum"`. The four boundaries coincide (wall-clock alignment is
inherited from `BucketTime`), so the compound-buffer join downstream sees
all four scalars line up per minute, no jitter.

## Follow-up

The saved-chart-slice demo currently uses bare atomic keys
(`openPrice`/`highPrice`/`lowPrice`/`lastPrice`) — running OHLC over
`taHistoryMax` that updates per tick, not per minute. Switching its seed
back to `pipeline_json` subscriptions driven by these nodes is a
separate proposal (the embassy ferrying path already supports it). See
`specs/2026-05-12-gma-window-nodes/PLAN.md` "Out of plan" for context.
