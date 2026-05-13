# Window nodes

> Phase 1 stub — covers `TumblingWindow` only. `VectorReducer` and the
> worked OHLC composition land in Phase 2 (proposal:
> `specs/2026-05-12-gma-window-nodes`).

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
    { "type": "TumblingWindow", "periodMs": 60000 }
    /* … further stages (e.g. VectorReducer in Phase 2) … */
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
