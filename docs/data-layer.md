# GMA_V3 Data Layer — ClickHouse selects, gma computes

**Status:** design. Nothing here is implemented. ENC-1478.
**Read §1 first.** It is one decision, and every other choice in this document is
downstream of it.

---

## 0. TL;DR

Raw Databento data lands in a ClickHouse **L1** store. There are no derived layers.
ClickHouse is used as indexed, compressed, columnar **storage and selection only** — it
never computes a value gma could compute. A **shim** between the two reads a gma manifest,
works out the minimal lossless `SELECT` that satisfies it, and streams column blocks into
gma with no row materialization. gma performs **100% of the computation**, through the
same operator code whether it is live, replaying, filling a chart, or running headless
research.

That boundary is the product requirement, not an architectural preference. A transpiler
that reimplemented gma's math in SQL would produce small numerical differences — float
accumulation order, bucket edge inclusivity, EMA seeding, gap handling — and this system's
whole business is reading small differences as signal. There is no tolerance at which
that is safe, so there is exactly one implementation of every operator and it lives in
gma.

Performance therefore comes from **transport, not semantics**: batching how values reach
gma's existing operators, which is bit-identical by construction, and sharding by
`(symbol, day)`, which is semantically free because gma's semantics are already
per-symbol.

---

## 1. The one decision

> **ClickHouse does selection. gma does computation. Nothing computes a value twice.**

### 1.1 Why a transpiler is not an option

The tempting design — compile a gma node graph to SQL, let ClickHouse execute it — fails
for a reason that has nothing to do with whether the compiler is correct.

Consider the chart-fill case. A manifest defines how a chart is filled. If the fill is
computed by translated SQL and the live continuation is computed by gma, the chart's
filled region and its live region were produced by **two different implementations**. Even
a flawless translation differs:

- float32 vs float64 accumulation, and summation order within an aggregate;
- window-boundary inclusivity at the bucket edge;
- EMA/RSI seeding over the first N samples;
- NaN, null and empty-bucket handling;
- tie-breaking among records sharing a timestamp.

None of those is a bug. All of them put a kink at the seam, and **this system exists to
notice differences of exactly that size and call them an indication.** A discrepancy
manufactured by the transport layer is indistinguishable from signal, so it is not a
performance trade or a precision trade — it poisons the output.

A diff-based parity gate does not rescue it either. Two engines will not agree
bit-for-bit on floating-point aggregation, so any such gate must carry a tolerance, and a
tolerance is precisely the noise floor we cannot accept.

**So the transpiler is out, and with it the "two executors behind one logical plan"
design.** One implementation of the math.

### 1.2 Why selection is safe

Selection is lossless *by definition*: a row either satisfies a predicate or it does not,
and no arithmetic is performed on the way. The shim may push down only operations with
that property:

| permitted | why it is lossless |
|---|---|
| `WHERE` on `symbol` / `ts_event` / `provider_seq` | set membership; no value changes |
| partition pruning (`toYYYYMMDD(ts_event)`) | skips files; no value changes |
| column projection | drops columns the manifest never reads |
| `ORDER BY` matching the physical primary key | no sort happens; the order is already on disk |
| `LIMIT` **at the query-layer boundary only** | a row cap on a raw page — see the hazard below |

| forbidden, without exception |
|---|
| any aggregate function (`avg`, `sum`, `min`, `max`, `count`, `argMin`, …) |
| any window function (`OVER`, `LAST_VALUE`, `exponentialMovingAverage`, …) |
| any arithmetic or expression in the projection list |
| `GROUP BY` of any kind |
| time bucketing (`toStartOfMinute`, `intDiv(ts, N)`, …) |

**`LIMIT` hazard.** A `LIMIT` is safe as a page cap on a raw row request. It is **not**
safe as a bound on a graph's input, because it truncates the warm-up region and changes
every value the graph emits. The planner may emit `LIMIT` only for a raw page, never for
a computation feed.

### 1.3 How the boundary is enforced

Not by a diff. By a **lint** on the generated SQL (§5.4): parse it and reject any
statement containing a construct from the forbidden list. That is a structural guarantee —
it fails at build time on the *shape* of the query rather than detecting a numerical
divergence after it has already shipped — and it is strictly stronger than any parity
tolerance.

---

## 2. What exists today

Mapped from `master` @ `90f0778`.

### 2.1 Ingest

```
TCP :9001 ──▶ FeedServer::handleLine ──┐
                                        ├──▶ Event{symbol, shared_ptr<rapidjson::Document>, type}
wss:// ──▶ WsFeedClient ──▶ IFeedAdapter│         │
            ::translate() ──▶ FeedEvent ┘         ▼
                   │                      Dispatcher::onTick
                   └──▶ OrderBookManager (book mutations, seq/epoch)
```

- **`engine::IIngressSource`** (`include/gma/engine/IngressRegistry.hpp`) — `start()` /
  `stop() noexcept`, with named factories on `IngressRegistry` instantiated from
  `cfg.ingress[]` by the composition root (ENC-31). The right seam; reused as-is.
- **`gma::feed::FeedEvent`** — a `variant` over `TickEvent`, `ObAdd/Update/Delete/Trade/
  TickSize/ResetEvent`. Already carries `timestampNs` and `optional<uint64_t> seq` on the
  OB variants. The canonical schema in §8 is derived from it rather than invented.
- **`gma::Event`** — `{symbol, shared_ptr<rapidjson::Document>, type}`. A JSON DOM.
  `Dispatcher::onTick` iterates `payload->MemberBegin()..MemberEnd()` per tick
  (`src/core/Dispatcher.cpp:138`); `MarketTickComputer::compute` does `HasMember` probes
  per tick (`connectors/market/src/MarketTA.cpp:342-367`).
- **`OrderBookManager`** — the L2/L3 machinery already exists: `onSeq(symbol, seq)`
  returning "may I apply this", per-symbol `FeedState{lastSeq, epoch, stale}`,
  `onReset(symbol, epoch)`, a `requestSnapshotFn` gap callback, and
  `onSnapshotPerOrder`/`onSnapshotAggregated` accepting a resume seq. Prices are
  `Price{int64_t ticks}` internally, quantized per symbol (`kDefaultTick = 1e-4`), but
  every public entry point is `double`.

### 2.2 DAG execution, and where it spends its time

- **`StreamValue{std::string symbol, ArgType value, int64_t bucketStartMs}`** — the value
  on every pipeline edge. Note the `std::string`: a symbol is **copied into every value**.
- **`INode::onValue(const StreamValue&)`** — one scalar, one virtual call, per node, per
  value.
- **`Dispatcher::deliver`** posts to `rt::ThreadPool` for every node that does not hold a
  strand; ENC-1005 gives each request DAG an `rt::Strand` and calls strand-bearing
  `Listener`s inline.
- **`AtomicStore`** — latest-value-wins `(symbol, field) -> ArgType` behind a
  `shared_mutex`, written per derived value. ENC-1340 bounded reader starvation against a
  queued writer at one yield.
- **Clocking.** `BucketTime` and `TumblingWindow` use `system_clock::now()` + `wait_until`
  (`src/nodes/BucketTime.cpp:70`, `src/nodes/TumblingWindow.cpp:76`); `Interval` uses
  `cv.wait_for` (`src/nodes/Interval.cpp:33`). Each owns a `std::thread` that fires
  whether or not data arrived.
- **Pull sampling.** ENC-1335: `samplesAtClock()` / `bindAtClock()` — an `AtomicAccessor`
  adjacent to a clock hop samples the store on the ingress thread, because that is the
  only place the store is coherent with the tick.
- **Egress.** `Responder` → `ClientSession` outbox, **deliberately lossy**: `update`
  frames coalesce above 256 queued and displace the oldest pending `update` at 4096
  (ENC-996 / ENC-1072).

### 2.3 What does not exist

No Arrow, Parquet, DuckDB or ClickHouse anywhere (`third_party/` holds only `rapidjson`).
No history, persistence, replay or backfill. Event timestamps are extracted
(`MarketTA.cpp:364`) and then stored as a **string** atomic (`MarketTA.cpp:392`) and read
by nothing.

### 2.4 Downstream, and two constraints that bind this design

- **embassy already owns the chart time axis.** `TimeBasis{baseMs, periodMs, epochKnown}`
  rides `createBuffer`; records carry a **bar ordinal**, not a time, and
  `ordinal = (bucketStartMs - baseMs) / periodMs` (`internal/pipeline/orchestrator.go:212`).
- **`baseMs` is frozen and ordinals are unsigned.** `orchestrator.go:787` is
  `r.basis.baseMs.CompareAndSwap(0, bucketStartMs)` — the first bucket ever seen, set
  once. It is frozen deliberately: embassy gates scene-init rebroadcast on a byte compare
  of the serialised envelope, so a changing `baseMs` re-runs `createBuffer` and **zeroes
  every connected client's buffer** (the ENC-1101 failure). With `ordinal uint32` that
  only moves up, **history before `baseMs` is not representable** (§10).
- **DynaCharting does not speak Arrow.** It consumes packed `float32` via `createBuffer` +
  `PackAppend(bufferID, offset, []float32)`. Arrow stops at gma/embassy.
- **No chart-fill path exists.** `customer-layer/src/api/history.ts` is *pipeline build*
  history (Observatory, ENC-825), not market data. Charts start empty and fill forward.

---

## 3. Where this conflicts with what exists

C1–C5 block. C6–C12 are real but local.

### C1 — Event time does not exist, and the clock nodes are wall-clock threads

`StreamValue` has no event-time field; its only time-ish member, `bucketStartMs`, is
documented as wall-clock-derived. All three timer nodes fire on the host clock
independent of data arrival.

Replay a 10-minute window through `TumblingWindow(60s)` and you get **one** bar, because
the replay finishes in under a second of wall time. A different *number* of outputs, so no
tolerance recovers it.

What it takes:
1. `StreamValue` gains `int64_t eventTimeNs`. It is an aggregate with default member
   initializers, so every `StreamValue{sym, val}` site still compiles — the property
   ENC-1280 relied on.
2. Every value-transforming node copies it through, as they already do `bucketStartMs`.
3. `TumblingWindow` / `BucketTime` get an **event-time driver**: a boundary is crossed
   when the watermark passes it, not when a thread wakes. The wall-clock thread survives
   in live mode only, as the "close a quiet bar" path, and is disabled in replay.
4. `bucketStartMs` is re-derived from event time. **Wire-visible change** against
   `specs/2026-09-20-timestamps-on-the-wire/SPEC.md` D9/D10, which pins it to the
   wall-clock boundary on purpose. That SPEC needs amending. → **Q1**.

This is the largest single piece of engine work and nothing about replay or research
parity is testable before it lands.

### C2 — The engine's ingress unit is a JSON DOM

`gma::Event` holds a `rapidjson::Document` walked member-by-member per tick. The shim
produces column blocks; serialising them to a DOM to reach `onTick` would be strictly
worse than today.

`Dispatcher` needs a second ingress entry point:

```cpp
void onTick(const Event& tick);                 // existing JSON path, unchanged
void onRecord(const data::TickRecord& rec);     // resolved fields, no DOM
```

`Event` and the JSON path stay for the legacy TCP / `market.wsclient` ingress. Two ingress
shapes is a cost; folding them is a bigger one.

**This is no longer a question about Arrow's reach.** Arrow (or ClickHouse's native column
blocks) is the shim's currency and stops at the shim's decode. gma's operators stay
scalar, because §1 forbids a second implementation and rewriting them *is* a second
implementation. What changes is only how values are delivered to them (§6).

### C3 — No sequence number on the engine value path, and none on `TickEvent`

`feed::TickEvent` has `timestampNs` and **no `seq`**. `gma::Event` has neither. So
dedup — at the splice, and at re-ingest — cannot be evaluated for trades or quotes.

`provider_seq` becomes **mandatory and non-optional** in the canonical schema. For
Databento it comes from the provider and is therefore stable across re-downloads, which is
what makes bulk re-ingest idempotent (§8.6). The synthesis path (§9.4) exists for
feed-simulator and future providers, not for Databento.

### C4 — Warm-up and replay are different modes, and the engine has neither

`MarketTickComputer::_symbolHistories` and `Dispatcher::_histories` are populated **only**
by `onTick`. There is no way to advance that state without firing every listener.

| mode | state | output |
|---|---|---|
| **warm-up** — prime a 200-period SMA before the window of interest | advance | **suppress** |
| **replay / research / fill** — produce values over the window | advance | **emit** |

Without the suppress mode, every chart receives a lookback of partially-warm values on
connect, and every research result is prefixed with garbage. → **Q3**.

Note this couples to the planner: the warm-up depth is not a config constant, it is
**derived from the graph** (§5.1).

### C5 — Output capture cannot be at the WebSocket boundary

`ClientSession`'s outbox drops `update` frames by design above 256 queued;
`docs/ARCHITECTURE.md` §8 names *event replay* as the case it breaks. A research run or a
chart fill that outruns its consumer loses frames **by design**.

Research output and parity recording are captured **upstream of the outbox**, at the
`Responder`'s send function, by a sink with no queue and no coalescing (§11).

### C6 — gma's hot path is transport-bound, and that is where the performance budget is

Per value, today: a `std::string` symbol copy, a virtual `onValue` call per node, a
`ThreadPool::post` hop for every non-strand node, an `AtomicStore` write under a
`shared_mutex` for every derived value, and a rapidjson DOM at ingress. For a chart tick
that is irrelevant. For a research scan over a month of raw L1 it is the whole cost.

§6 is the plan. It is all transport and touches no operator.

### C7 — Deterministic execution needs an explicit quiesce, not a sleep

`Dispatcher.hpp` records single-threaded ingress as a *precondition* of ENC-1005's
ordering, not an enforced property — "one `for (...) threads.emplace_back([&]{ioc.run();})`
away" from breaking. Replay and research runs must assert it, set `threadPoolSize = 1`,
and end on `ThreadPool::drain()` — the deterministic barrier ENC-1340 established. A
longer sleep is "not a fix, it is a wider window".

Inline DAG drive (§6) makes this cheaper rather than more expensive: there is no pool to
drain on the fast path.

### C8 — `IIngressSource` has no completion signal and no clock

`start()`/`stop()` is right for an endless feed and wrong for a replay, which *finishes*,
and carries no event-time clock. Both additions live on a `gma_data` interface that
adapts *to* `IIngressSource`, so wiring stays an INI edit (§4).

### C9 — Fixed-point scale: DBN is global 1e-9, gma's is per-symbol runtime

Databento DBN prices are `int64` at a fixed 1e-9 scale. `OrderBookManager` quantizes to
`Price{int64_t ticks}` with a per-symbol runtime tick (default `1e-4`) and `double` public
entry points.

The canonical schema uses a **single global 1e-9 scale** — no per-row scale column, no
per-symbol lookup on decode, and an exact integer path from DBN with no double round
trip. Per-symbol tick size stays a book-quantization concern. The remaining
`int64 → double` at the `OrderBookManager` entry points wants `Price`-typed overloads
eventually; deferred, not blocking. → **Q4**.

### C10 — ClickHouse is a required service, and that contradicts the original footprint rule

The brief said "no new required services in the default deployment". With L1 in
ClickHouse, history requires one. Resolution is **two deployment profiles** rather than
one compromise:

| profile | store | footprint |
|---|---|---|
| **research / ingest** (your infrastructure) | ClickHouse L1 | ClickHouse is a required service, by design |
| **client deployment** | live-only, or reads a central L1 over the wire | gma + an HTTP/native client; no local store |

Both are `IL1Store` configurations plus two build profiles, which is what the
interface was for. This also resolves the sovereignty-vs-central-hosting contradiction
(old Q11) into a profile choice rather than an either/or.

**And the dependency budget improves.** With no Parquet store there is no DuckDB — the
largest single dependency, and it existed only to query Parquet (§15).

### C11 — `AtomicStore` is mutable latest-value state, and pull nodes race it

ENC-1335 exists because the store is written by the ingress thread one statement before
the clock is posted. The `bindAtClock` fix binds the sample to the clock, but **only at
the two adjacency points**; a pull node deeper in a chain keeps execution-time sampling by
design. So a graph containing such a node is not bit-reproducible, and that is existing
documented behaviour rather than something this design introduces. Research graphs and
parity fixtures must either avoid it or record it as expected. → **Q5**.

Inline DAG drive (§6) narrows this considerably — with no pool hop between the store write
and the sample, the race window closes on the fast path — but it does not close it as a
*contract*, and the contract is what matters.

### C12 — `ReplacingMergeTree` dedup is only exact if the sort key is unique

Idempotent re-ingest (§8.6) relies on ClickHouse collapsing rows with an equal `ORDER BY`
key. For `trades` and `quotes`, `(symbol, ts_event, provider_seq)` is unique if the
provider's seq is per-symbol monotonic.

**For `book_events` it is not.** One DBN message can yield several events — an
order-executed produces a book mutation *and* a trade print — so two distinct rows can
share `(symbol, ts_event, provider_seq)` and `ReplacingMergeTree` would **silently
collapse them**, deleting real data on an ordinary background merge. The sort key for
`book_events` therefore needs a discriminator (`action`, `order_id`, or an intra-message
ordinal). Named here because the failure is silent, delayed, and indistinguishable from a
gap. → **Q6**.

---

## 4. Components and data flow

```
┌─ Databento (bulk historical DBN; live deferred — plan limits) ───────────────┐
└──────────────────────────────┬──────────────────────────────────────────────┘
                               ▼
┌─ gma_data ──────────────────────────────────────────────────────────────────┐
│  IProviderAdapter          INormalizer                                      │
│  ┌──────────────┐          ┌──────────────────────────────────────┐         │
│  │ Databento    │   raw    │ decode → canonical columns →          │         │
│  │ DBN          │ ───────▶ │ column blocks                         │         │
│  ├──────────────┤  frames  │  (ts_event, provider_seq, session_id, │         │
│  │ feed-sim     │          │   symbol, venue)                      │         │
│  └──────────────┘          └───────────────┬──────────────────────┘         │
│                                            │ bulk insert, symbol-sorted      │
│                                            ▼           per day partition     │
│                            ┌────────────────────────────────────┐           │
│                            │  ClickHouse L1 — RAW ONLY          │           │
│                            │  storage + selection. NO compute.  │           │
│                            │  MergeTree, PK (symbol, ts, seq)   │           │
│                            │  + projection (ts, symbol)         │           │
│                            └────────────────┬───────────────────┘           │
│                                             │                               │
│  ┌──────────────────────────────────────────▼────────────────────────────┐  │
│  │ THE SHIM                                                              │  │
│  │   planner  — manifest → minimal lossless SELECT, index-aware,         │  │
│  │              shard-aligned, warm-up depth derived from the graph      │  │
│  │   lint     — reject any generated SQL containing compute (§5.4)       │  │
│  │   transport— column blocks, zero-copy, batched, back-pressured        │  │
│  │   decode   — columns → POD, symbol interned. ONE conversion point.    │  │
│  └──────────────────────────────────────────┬────────────────────────────┘  │
│                                             │ TickRecord / BookRecord        │
│  ISource:  LiveSource │ ReplaySource │ SplicedSource (§9)                   │
└─────────────────────────────────────────────┼──────────────────────────────┘
                                              ▼
┌─ gma_engine — ALL COMPUTATION, ONE IMPLEMENTATION ──────────────────────────┐
│  Dispatcher::onRecord ──▶ IEventComputer ──▶ AtomicStore                     │
│                       └─▶ Listener ──▶ … event-time clocked nodes … ──▶      │
│                                     Responder ──▶ WS (charts)                │
│                                     SinkNode  ──▶ Arrow table (research)     │
│                                     RecordingSink ──▶ parity fixture         │
└─────────────────────────────────────────────────────────────────────────────┘
```

Six rules:

1. **ClickHouse computes nothing semantic.** §1. Enforced by the lint, not by a diff.
2. **One implementation of every operator**, in gma, shared by live, replay, chart fill
   and research.
3. **gma owns no durability.** It reads L1; it never writes the raw path.
4. **One conversion point** — column blocks → POD, in the shim's decode, column-wise.
5. **The splice lives in `ISource`** and nowhere else. No graph branches on "am I live".
6. **`gma_data` depends on `gma_engine`, never the reverse.** Arrow and the ClickHouse
   client must not become engine dependencies.

`gma_data` is a peer of `connectors/market/`: the market connector owns book and TA
semantics, `gma_data` owns transport, encoding and selection. The market connector will
depend on `gma_data` for the record types.

---

## 5. The shim

The centerpiece, and the hard design. Its job is to make ClickHouse behave as an indexed
column store for gma's benefit, and to get rows into gma fast enough that gma's own
compute is the bottleneck rather than the I/O.

### 5.1 The planner

**It analyses the graph's data dependencies. It does not translate the graph's math.**
That distinction is the whole design.

Given a manifest (the same JSON node graph `TreeBuilder` consumes) plus a requested
window, the planner derives:

| derived | from | why it matters |
|---|---|---|
| **symbols** | the `streamKey`s the graph's `Listener`s bind | predicate on the PK's leading column |
| **time range** | requested window **minus warm-up depth** | see below |
| **columns** | the `field`s the graph reads, mapped through `MarketFieldMap` | a column store charges per column read; `SELECT *` throws the layout away |
| **record kinds** | which of trades / quotes / book_events the graph touches | avoids scanning tables the graph never reads |
| **ordering** | always `(symbol, ts_event, provider_seq)` | matches the physical key, so no sort executes |
| **shards** | `(symbol, day)` tiles over the range | §7 |

**Warm-up depth is derived, not configured.** A graph containing `sma_200` over 1-minute
buckets needs 200 extra minutes of input before the window start, or its first 200 output
values are wrong. The planner computes this by walking the graph for the deepest history
requirement across every operator and bucket period. This subsumes what §9 would
otherwise carry as a `lookbackNs` config knob, and it is the only place that number can be
correct — a human setting it per request will get it wrong, silently, in the direction of
too little.

Output: one `SELECT` per `(kind, shard)`, each of which the primary key serves directly.

### 5.2 Transport

Requirements, in priority order:

1. **Column blocks, not rows.** ClickHouse's native protocol speaks columns; HTTP
   `FORMAT ArrowStream` speaks Arrow. Either avoids row materialization. **Which is faster
   is unmeasured and is benchmark B1 (§14).** This reverses an earlier lean toward HTTP:
   that was correct when the backend was optional, and the shim's throughput is now the
   entire performance story.
2. **Zero-copy into gma's address space** where the wire format permits it.
3. **Bounded buffering with back-pressure.** ClickHouse will read faster than gma
   computes. Without a bound, a month-long research scan buffers a month in RAM.
4. **Order preservation across shards.** Shards are disjoint per `(symbol, day)` and gma's
   semantics are per-symbol, so shards need no cross-merge — but within a shard the stream
   must stay in PK order, which means no multi-part merge that interleaves.
5. **Batch size tuned to cache**, not to convenience: large enough to amortize per-batch
   overhead, small enough to stay resident.

### 5.3 Decode

One function, column-wise, producing `TickRecord` / `BookRecord` (§8.7). Two details that
matter more than they look:

- **Symbols are interned to an id on decode.** `StreamValue` carries
  `std::string symbol`, so today a symbol is heap-copied into every value on every edge.
  At research scale that single copy is a measurable fraction of total cost.
- **No DOM is constructed anywhere on this path.** This is the `onRecord` entry point
  (C2), and it is a net reduction in per-tick work versus the shipped JSON ingress.

### 5.4 The lint — the structural guarantee

Every `SELECT` the planner emits is parsed and checked against the forbidden list in
§1.2 before it is sent. A violation is a **build-time failure in CI**, not a runtime
warning:

```
gma_shim_sql_lint:  planner output for corpus graph 86 contains
                    aggregate function 'avg' in projection list
                    -> computation must not leave gma (docs/data-layer.md §1)
```

Run over the whole request corpus (`tests/treebuilder/corpus_requests.json`, 272 entries),
this asserts that no manifest in the corpus can cause the planner to push computation
down. It replaces a parity tolerance with a property, which is why §14's test plan is
shorter and stronger than a two-engine diff would have been.

**Anti-vacuity:** the lint must also assert it *parsed* and *saw* the statements — a lint
that silently matched zero queries reads exactly like a passing one. It asserts a non-zero
statement count and includes a deliberately-violating fixture registered `WILL_FAIL`.

---

## 6. gma's fast path — transport, not semantics

The performance budget, and **none of it touches an operator**. Each row is bit-identical
by construction because it is the same function called in the same order with the same
arithmetic; the only change is what happens *between* calls.

| change | mechanism | semantic risk |
|---|---|---|
| no JSON DOM at ingress | `Dispatcher::onRecord` (C2) | none |
| no `ThreadPool` hop between nodes | drive the DAG **inline** per record on one thread | none — the same calls in the same order, and it is what C7's determinism wants anyway |
| no symbol heap copy per value | intern to an id in the shim's decode (§5.3) | none |
| no `AtomicStore` write per derived value | write only what a pull node actually reads | none |
| amortized dispatch | batch the **drive loop**; operators unchanged | none |

Measure in that order and stop when gma is faster than the scan. **The first four require
no node changes at all**, which is both the cheapest work and the lowest-risk.

If that is not enough, the next step is **opt-in per-node batch paths** — e.g. a `Worker`
that consumes a contiguous span instead of N scalars — gated by an **exact** equality
assertion against the scalar path over the corpus. Exact equality is achievable here
precisely because it is the same arithmetic in the same order; that is the property a SQL
transpilation could never offer, and it is why this is a legitimate optimization and that
was not.

**Explicitly not on the table:** reimplementing any operator's math in a vectorized form
that changes accumulation order. That is a second implementation wearing a different hat.

---

## 7. Scale: shard, don't transpile

One implementation means gma touches every row. A month of raw L3 book events is billions
of rows, and no amount of transport tuning makes a single thread interactive over that.

The answer that preserves identical semantics: **gma's semantics are already per-symbol**,
so partition the work by `(symbol, day)`, run N independent gma computations, concatenate
the outputs. Each shard is an ordinary complete gma run — no new code path, no merge
semantics, nothing to diverge. Throughput is bought with cores instead of with a second
engine.

Consequences:

- The planner emits shard-aligned queries (§5.1), each hitting the PK cleanly.
- Cross-symbol operators are the exception and must be declared. `Aggregate`/`JoinBy` with
  `by: "streamKey"` is per-symbol and shards fine; a cross-symbol join (ENC-1292's
  `by: "none"`) does not, and such a graph runs unsharded or on a symbol-group shard.
  The planner must detect this from the graph, not trust the caller. → **Q7**.
- Per-shard determinism is the single-threaded contract (C7) applied per shard, which is
  stronger and simpler than trying to make one multi-threaded run deterministic.

---

## 8. Canonical schema — L1 only

Field definitions: `data/include/gma/data/Schema.hpp`. DDL: `data/schema/clickhouse.sql`.

### 8.1 There are no derived layers

L1 holds raw Databento records. There is no L2, no rollup, no materialized bar table, no
feature store. The reason is sound: research is ad hoc with no stable access pattern, so
a derived layer is a bet on queries nobody has written yet — and under §1 any *computed*
layer would be a second implementation besides.

**One place this costs something measurable**, recorded so the trade is explicit rather
than discovered: a long-interval chart fill — 1000 × `1d` bars — scans roughly three
years of raw trades to return 1000 rows. 1000 × `1m` is fine. So it is not "charts are
slow", it is specifically the zoomed-out end. → **Q8**.

### 8.2 What is allowed, because it is not a layer

**ClickHouse projections.** Same table, same columns, no new semantics — a second physical
sort order maintained on insert and chosen automatically by the optimizer. That is an
index, not a derived model, and it is the only way to serve two access patterns off one
raw store:

| reader | wants |
|---|---|
| replay, chart fill, sharded research | `(symbol, ts_event, provider_seq)` — the primary key |
| cross-sectional research (all symbols, narrow window) | `(ts_event, symbol)` — a projection |

With symbol leading, `ts_event` is sorted only *within* a symbol, so "everything in this
ten-minute window" reads every symbol's granules in the day partition. The projection
fixes that without committing to any derived model. `minmax` skip indexes on `ts_event`
are nearly free and worth having too.

### 8.3 Common columns

| column | type | notes |
|---|---|---|
| `ts_event` | `DateTime64(9,'UTC')` | venue event time. **The only time an operator may key off.** |
| `ts_recv` | `DateTime64(9,'UTC')` | normalizer receipt. Diagnostics only; never an operator input, never a window boundary. |
| `provider_seq` | `UInt64`, non-null | monotonic within `(session_id, symbol)`. From Databento; synthesized only for providers without one. |
| `session_id` | `UInt32` | ingest session. Dedup key is the **pair** `(session_id, provider_seq)`. |
| `symbol` | `LowCardinality(String)` | thousands of values, billions of rows. |
| `venue` | `LowCardinality(String)` | Nasdaq / OTC / … |

Prices are **`Int64` at a fixed global 1e-9 scale** (C9). No per-row scale column.

### 8.4 Tables

Three event tables, not one union table: a unified table needs every type's fields
nullable, which defeats the codecs and makes every read scan columns it does not want.

- **`trades`** — `price`, `size`, `aggressor`, `book_neutral`, `trade_id`, `flags`.
  `book_neutral` mirrors `ObTradeEvent::bookNeutral`: a print that must **not** consume
  resting liquidity. Dropping it double-counts volume against the book.
- **`quotes`** (L1 top-of-book) — `bid_px`, `ask_px`, `bid_sz`, `ask_sz`, `bid_ct`,
  `ask_ct`, `flags`. One row per TOB change. An absent side is `kNoPrice` (`INT64_MIN`),
  not `0` and not `NULL`: `0` is a legal price and negative prices are legal
  (`setAllowNegativePrices`), and `Nullable` costs a mask on the hottest column.
- **`book_events`** (L2/L3) — `action`, `side`, `price`, `size`, `order_id`, `priority`,
  `depth`, `epoch`, `flags`. `action` mirrors the `feed::FeedEvent` variant set one-for-one.
- **`gaps`** — detected discontinuities as rows, not metrics. A research run that cannot
  see "there was no data here" treats a gap as a flat market, which is a wrong-answer
  failure rather than a missing-telemetry one.

### 8.5 `book_snapshots` — the one argued exception

Replaying an arbitrary window needs a book seeded at its start. Without a persisted
snapshot that means replaying every delta from session open — O(session) *per research
run*, which at research scale is the dominant cost.

A snapshot is materialized derived state, so it brushes against §8.1. The argument for it:
it is raw **book state**, not an aggregate — no operator semantics are involved in
replaying deltas into a book, and `OrderBookManager` is the one implementation of that
either way. The argument against: it is still something written once and trusted later.

Not assumed. → **Q9**, with the cadence question (every N deltas / N seconds of event
time / session boundaries) riding on it.

### 8.6 Engine and idempotent re-ingest

```sql
ENGINE = ReplacingMergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
```

Bulk ingest means re-pulling windows — a gap, a correction, a re-download — **will**
happen. `ReplacingMergeTree` collapses rows with an equal sort key, and because
`provider_seq` comes from Databento and is stable across downloads, re-ingesting a window
converges to the same rows. That is a real operational property, and it is the strongest
argument for not synthesizing seq for Databento.

**See C12**: this is only exact where the sort key is unique, and for `book_events` it is
not. That table needs a discriminator in the key or it will silently lose rows on a
background merge.

Insert **symbol-sorted batches per day partition**. The key leads with symbol, so raw
DBN file order (time-ordered, multi-symbol) forces a per-part re-sort on insert. The day
partition bounds it, but sorting on the way in keeps merges cheap.

### 8.7 Engine-facing PODs

`RecordHeader{tsEventNs, tsRecvNs, providerSeq, sessionId, symbolId, venueId}` plus
`TickRecord` and `BookRecord` — the output of §5.3's single decode. `symbolId` rather than
a string, per §5.3.

### 8.8 Codecs

`DoubleDelta` on `ts_event` / `ts_recv` / `provider_seq`; `Delta(8), ZSTD(3)` on prices;
`T64, ZSTD(1)` on sizes. Rationale and the full DDL are in
`data/schema/clickhouse.sql`.

---

## 9. The live/historical splice — designed, demoted

**Demoted, deliberately.** Databento Plus is not affordable, so there is no live Databento
feed, so nothing in the research or chart-fill path splices. The only thing exercising
`SplicedSource` is feed-simulator. It remains designed because chart-attach-then-live needs
it and live arrives when the plan tier does, but it is **not on the critical path** and
should not lead the ticket order.

Named risk: the splice will ship exercised only against feed-simulator. That is a
deterministic, good fixture, and it is not a real provider.

### 9.1 States

```
BUFFERING ──▶ BACKFILLING ──▶ REPLAYING ──▶ DRAINING ──▶ LIVE
    │                                                     ▲
    └──────────────── gap / disconnect ───────────────────┘
                              └──▶ RESYNC ──▶ BACKFILLING
```

Two orderings are load-bearing:

- **Subscribe before you query.** `BUFFERING` opens the live subscription *first* and
  records the first live `(seq, ts)` per symbol as the splice point. Query first and the
  window between "history ends" and "live begins" is lost with no error anywhere.
- **Dedup on the pair.** `DRAINING` drops records where
  `(session_id, provider_seq) <= lastReplayed`. A restart resets a synthesized seq to 0,
  so comparing bare seqs across a restart silently re-admits replayed rows.

### 9.2 Per-symbol, not per-source

`AAPL` may be `LIVE` while `MSFT` is still `BACKFILLING`. A source-wide state machine
stalls every symbol behind the slowest, which on an OTC feed may be a symbol that does not
print for an hour. Source state is the minimum over symbols.

### 9.3 Buffer bound

Overflow during a slow backfill has three options and only one is safe: drop-oldest and
drop-newest both punch a silent hole, so overflow **aborts and retries** with a later `T`.
Each abort increments a counter — a deployment aborting repeatedly has a backfill too slow
for its feed rate, and an operator must see that rather than a quietly thin chart.

### 9.4 Sequence synthesis

For a provider with no monotonic seq, the normalizer assigns a per-`(symbol, session)`
counter at ingest. Exact for everything downstream of that normalizer, and **not**
comparable across restarts — hence `session_id`. Databento does not need this path.

### 9.5 L2/L3 gap recovery

`OrderBookManager` already implements it: `onSeq` gates each delta and returns false on a
gap, marks the symbol stale, and invokes `requestSnapshotFn`. **The data layer supplies
that callback** — read the nearest snapshot from L1 (replay) or request one from the
provider (live). Same callback, two implementations, chosen by the source. `onReset` bumps
the epoch that `OrderKey` already carries, so pre-reset orders cannot be matched by
post-reset messages.

A stale book must not serve `ob.*` reads as fresh. `isStale(symbol)` exists; whether
`ob::Provider` consults it is **unverified** and worth checking during implementation.

---

## 10. Chart fill

The same query layer, with one downstream constraint that decides the design.

### 10.1 The binding constraint

Per §2.4: `baseMs` is the first `bucketStartMs` ever seen, set once by CAS and frozen
because changing it rebroadcasts scene-init and **zeroes every connected client's buffer**
(ENC-1101). Ordinals are `uint32` and only move up.

> **History before `baseMs` is not representable.** You cannot fill backwards from an
> established basis.

So the **fill depth must be known at scene-init**, with `baseMs` seeded from the start of
the filled window rather than from the first live bar. That is a one-line change at
embassy's CAS — seed it from the query instead of from the first live value — and
`createBuffer` is pre-sized to history + live headroom, which the wire already supports
(`SceneBuilder.ts:457` passes `byteLength`).

**Consequence:** infinite scroll-back is not expressible on the current wire.
Attach-with-N-bars is. → **Q10**.

### 10.2 Keyed by pipeline, not by symbol

A chart is not "OHLC for AAPL". forum/aibox compiles to a DC scene whose slots bind to gma
`requestId`s — `candles-with-overlays-v1` binds `ohlc.open/high/low/close`, `vwap`,
`sma20`, `volume`. Filling it means filling **seven series**, each a pipeline's output.
Under §1 every one of them is computed by gma, from L1, through the same operators the
live stream uses. That is the entire reason the transpiler was rejected.

### 10.3 The wire

- A **`query` request type** on gma's WS protocol. `ClientSession` handles only
  `subscribe`/`cancel` today.
- The response must be a **lossless** frame class. A 50k-bar fill through the coalescing
  `update` outbox is exactly the ENC-996 failure (C5).
- **Arrow stops at embassy.** DynaCharting consumes packed `float32` via `PackAppend`, so
  the Arrow → `float32` pack is a real server-side step (§2.4).

### 10.4 Chart attach is the splice at the client boundary

Fill to `T`, then live from `T`, dedup at the join — the same state machine and the same
`(session_id, provider_seq)` dedup as §9. If the two use different code, the seam produces
a doubled or missing bar, which is the classic charting bug and also a false indication.

---

## 11. Sinks and research output

A `SinkNode` is an ordinary `INode` registered on `NodeTypeRegistry` as `"Sink"`, so it is
specifiable from a JSON graph exactly like a `Responder` and `TreeBuilder` needs no
special case. It accumulates emitted `StreamValue`s into an Arrow builder and flushes a
batch on a row-count or **event-time** interval trigger. A wall-clock flush would produce
different batch boundaries under replay than live for identical input — the same defect
class as a wall-clock window, one layer out.

Three uses:

- **Research output.** The headless path: `ReplaySource` over L1 → DAG → `SinkNode` →
  Arrow table. This is "gma as the query engine transforming tables".
- **`RecordingSink`** — fixed schema `(request_key, symbol, event_time_ns,
  bucket_start_ms, value)`, installed at the `Responder`'s send function, upstream of the
  lossy outbox (C5). The parity instrument (§14). `value` is text, not a float column:
  `ArgType` is a variant over bool/int/double/string/vector/`Record`, the corpus has
  `Record` terminals, and a float column would silently make an exact comparison
  approximate.
- **Derived series persistence (optional).** Writing a computed series back so a later
  chart fill reads it instead of recomputing. **This is now optional rather than
  load-bearing**: under §1 the recompute path is the same code, so reading a persisted
  copy is a cache, not a correctness requirement. If it is added, the cache must be
  invalidated by graph identity — a persisted `sma20` from a changed manifest is a wrong
  answer with a plausible shape. → **Q11**.

Raw durability is never a sink's job; that is the normalizer's, upstream of gma.

---

## 12. Failure modes

### 12.1 Ingest: re-pull and correction

Expected, not exceptional. `ReplacingMergeTree` on a stable `provider_seq` makes a re-pull
converge (§8.6) — **subject to C12**, which must be fixed first or re-ingest silently
deletes `book_events` rows.

### 12.2 Partial insert

A ClickHouse insert is atomic per block, not per batch. A batch interrupted mid-way leaves
some blocks committed. Recovery is re-running the batch, which §12.1 makes safe. The
ingest job must therefore be **restartable from a window boundary** and record which
windows are complete — a side table, not an inference from row counts.

### 12.3 Gap in Databento coverage

Distinguish three cases, because they look identical in a `SELECT`:

| cause | detection | record |
|---|---|---|
| the market was quiet | no seq discontinuity | nothing — this is data |
| the provider has a hole | seq discontinuity | a `gaps` row |
| **we have not ingested it yet** | window not in the completeness table (§12.2) | not a gap — a missing ingest |

The third is the dangerous one: a research run over a not-yet-ingested window returns a
flat market and no error. Queries must be bounded by ingest completeness and **fail**
rather than return a short answer.

### 12.4 Provider disconnect (live, when it exists)

`WsFeedClient` reconnects with backoff to 30s, but the feed resumes at an unknown point, so
the gap is real even though the socket recovered. The source re-enters `RESYNC` →
`BACKFILLING` from `lastDeliveredEventTime` — the ordinary splice, run again.

### 12.5 Out-of-order records

- **Within a symbol from one provider:** should not happen. Count it, log it, pass it
  through in arrival order. Reordering on a bounded window hides the defect and adds
  latency.
- **Across sources at a splice:** handled by the exact seq-based drop in `DRAINING`, not by
  a reorder buffer.

### 12.6 Restart

gma holds no durable state and re-runs from L1. The normalizer restarts with a new
`session_id`. ClickHouse is the durable boundary; whether the raw DBN files are retained so
L1 is rebuildable is **Q12**.

### 12.7 ClickHouse unavailable

Research and fill fail loudly. A live-only client profile (C10) continues streaming with no
history and must report "no history available" rather than serving a thin window —
silently serving less data than requested is the failure mode this whole document is
organised against.

---

## 13. In scope vs deferred

**In scope:**
- `gma_data`: Databento DBN adapter, normalizer, bulk ClickHouse L1 ingest with
  restartable windows + a completeness table.
- Canonical schema (§8) + DDL, with C12 resolved before first ingest.
- **The shim**: planner, lint, transport, decode (§5).
- Event time in `StreamValue` + event-time drivers for `TumblingWindow` / `BucketTime`
  (C1). Several tickets, and the critical path.
- `Dispatcher::onRecord` (C2).
- gma fast path: inline drive, symbol interning, store-write elision (§6).
- Sharding by `(symbol, day)` + cross-symbol detection (§7).
- `ReplaySource` as an `IngressRegistry` kind; `SinkNode`; `RecordingSink`.
- Warm-up emission gate (C4).
- Chart fill: `query` request type, lossless response frames, embassy `baseMs` seeding,
  pre-sized `createBuffer`.
- Benchmarks B1–B4 (§14), before most of the above.

**Deferred, named:**
- Any derived layer (§8.1). Projections and skip indexes are not that (§8.2).
- `SplicedSource` live half — nothing exercises it until Databento live (§9).
- Live TotalView / Databento Plus.
- Per-node batch fast paths (§6) — only if §6's transport work is insufficient.
- Parquet / DuckDB entirely (C10, §15).
- Multi-provider merge for one symbol. `venue` keeps it open; conflict resolution is not
  designed.
- Corporate actions, adjusted prices, symbol-change mapping. A reference-data layer, not
  this one.
- Retention / TTL policy for L1.
- `Price`-typed `OrderBookManager` overloads (C9).
- Infinite chart scroll-back (§10.1).

---

## 14. Test and benchmark plan

### 14.1 The structural guarantees

These replace what a two-engine parity diff would have had to do with a tolerance, and
they are stronger because they constrain shape rather than measure outcome.

- **`gma_shim_sql_lint`** (§5.4) — no generated `SELECT` over the 272-entry corpus
  contains an aggregate, a window function, `GROUP BY`, or an expression in the projection
  list. Asserts a non-zero statement count, and ships a deliberately-violating fixture
  registered `WILL_FAIL TRUE` so the lint cannot pass vacuously.
- **Exact equality for any batch fast path** (§6) — a per-node batch implementation must
  produce bit-identical output to the scalar path over the corpus. Exact, not approximate,
  because it is the same arithmetic in the same order.

### 14.2 Replay parity

Still needed, and now narrower: it proves **live output equals replay output** through one
implementation, i.e. that event time and the source layer are correct. It is no longer
carrying the weight of cross-engine agreement.

Record once, by hand, from feed-simulator (deterministic, already the default local
source): `raw.arrows`, `expected.arrows`, `graphs.json`. CI loads the raw batches into a
temp ClickHouse (or `FixtureL1Store`), builds the graphs through the
**ordinary `TreeBuilder`**, drives a `ReplaySource` with `threadPoolSize = 1`, drains, and
diffs on `(request_key, symbol, event_time_ns, bucket_start_ms, value)`. Byte-identical —
no tolerance.

Gates, so it cannot decorate:

- **Anti-vacuity**: `expected` is non-empty, spans more than one `bucket_start_ms`, and
  contains output from a windowed DAG. Without this a replay emitting nothing passes every
  row comparison.
- **A falsification case that must fail**: `gma_replay_parity_wallclock`, registered
  `WILL_FAIL TRUE`, runs the same fixture with the event-time driver disabled so windows
  use the wall clock. It must mismatch, and **the test asserts the mutation actually
  applied** — a no-op mutation reads exactly like a passing gate.

Known non-determinism to pin before trusting the diff: pull nodes not adjacent to a clock
hop (C11), `Interval`-clocked graphs (no event-time analogue — it is a wall-clock cadence
by construction, → **Q13**), and multi-symbol graphs where `_listeners`' outer
`unordered_map` ordering can surface (key the diff by symbol, not by arrival).

### 14.3 Benchmarks — run these first

Nothing in §6 or §5.2 should be chosen without them, and `GMA_V3/benchmarks/` has no query
path target today. **No numbers in this document are measured.**

| id | measures | why |
|---|---|---|
| **B1** | rows/sec ClickHouse → gma address space, **transport only, no compute**. Native protocol vs HTTP `ArrowStream`. | sets the ceiling, and settles §5.2's open transport choice |
| **B2** | rows/sec **through the DAG**: today's JSON path vs `onRecord` vs inline drive vs interned symbols | the §6 budget, change by change, so each is justified separately |
| **B3** | end-to-end chart fill p50/p99 at 1000 bars, `1m` / `1h` / `1d`, cold and warm | the user-visible number, and §8.1's cost at `1d` |
| **B4** | shard scaling: throughput vs cores, `(symbol, day)` tiles | whether §7 actually buys interactivity |

**The target is explicit: B2 ≥ B1.** If gma computes slower than ClickHouse can deliver,
gma is the bottleneck and that is where the work goes. That is a well-defined engineering
goal, unlike "faster than SQL" — under §1 we are not competing with SQL, we are using it
as an indexed disk.

---

## 15. Dependency budget

Better than the previous revision, because dropping Parquet drops DuckDB.
**These are estimates and must be measured on this toolchain.**

| dep | why | verdict |
|---|---|---|
| Arrow C++ | canonical in-memory/IPC format; the shim's currency and the research output type | required; trim hard with `ARROW_*=OFF` |
| ClickHouse client (`clickhouse-cpp` **or** an HTTP client) | the only transport to L1 | required; which one is **B1** |
| Parquet | — | **dropped** (no Parquet store) |
| DuckDB | — | **dropped**. Was the largest dependency and existed only to query Parquet (old Q14, now answered) |
| Databento client | could be avoided by decoding DBN directly | **Q14** |

Against the minimal-footprint constraint: the client deployment profile (C10) needs Arrow
plus a wire client and no store at all, which is a far smaller payload than
Arrow+Parquet+DuckDB would have been. Build `gma_data` as a separately linkable library so
a live-only deployment omits it entirely.

---

## 16. Open questions

Q1–Q4 block the first tickets.

**Q1 — `bucketStartMs` under event time.** Event-time windows change it from a wall-clock
boundary to an event-time boundary — wire-visible against
`specs/2026-09-20-timestamps-on-the-wire` D9/D10, which pins it to the wall clock on
purpose. Amend that SPEC, or carry both bases? I would amend: two time bases on one frame
is how charts disagree with themselves.

**Q2 — ClickHouse Cloud or self-hosted?** Decides the transport candidates for B1,
projection and `ReplacingMergeTree` behaviour, and whether the client profile reads L1
directly or through forum. Still open and it affects §5.2.

**Q3 — Warm-up emission.** Confirm warm-up suppresses output and replay/research emits it
(C4, §5.1).

**Q4 — Fixed-point scale.** Confirm a single global 1e-9 scale, no per-row scale column
(C9).

**Q5 — Determinism scope.** Is bit-identical output promised for *all* graphs, or only for
graphs avoiding the documented execution-time-sampling case (C11, ENC-1335)? The honest
answer today is the latter, and the fixture should say so out loud.

**Q6 — `book_events` sort key.** What discriminates two events from one DBN message —
`action`, `order_id`, or an intra-message ordinal? **Must be answered before the first
ingest**, because C12 fails silently on a background merge and looks like a gap.

**Q7 — Cross-symbol graphs and sharding.** The planner must detect a cross-symbol join
(`by: "none"`, ENC-1292) and refuse to shard it. Does such a graph run unsharded, or on a
symbol-group shard, and who picks the group?

**Q8 — Long-interval fill.** Accept that 1000 × `1d` scans years of raw trades, or make
the one narrow exception to §8.1? B3 should inform this rather than a guess.

**Q9 — Book snapshots.** The one materialized-state exception (§8.5): keep them, and at
what cadence — or rebuild from session open per research run?

**Q10 — Chart fill depth at scene-init.** Accept bounded attach-with-N-bars for V1, or
change the wire to support rebasing/negative ordinals? I lean accept: it is a one-line
embassy change, and rebasing means solving the buffer-zeroing problem (ENC-1101).

**Q11 — Persist derived series as a cache?** Now optional rather than load-bearing (§11).
If yes, the invalidation key is graph identity, and getting that wrong yields a
plausible-looking wrong answer.

**Q12 — Is L1 the only copy of the Databento pull?** If the raw DBN files are retained,
L1 is rebuildable and §12.6 carries much less.

**Q13 — `Interval` under event time.** Does it get an event-time mode, or is it defined as
live-only and excluded from replay and research? It is a wall-clock cadence by
construction.

**Q14 — Databento client library, or decode DBN directly?** DBN is the schema reference
either way. Their client is faster to stand up; decoding ourselves keeps the adapter
boundary honest and the dependency list shorter.

---

## 17. Project split

This is three projects, not one. Each wants its own Linear project and `specs/` directory
per workspace guardrail 4; they are listed here so the split is a decision rather than an
accident.

**P1 — Data layer.** Databento adapter, normalizer, L1 schema + DDL, restartable bulk
ingest, completeness table, `ReplaySource`. Repo: GMA_V3.

**P2 — Query layer and the shim.** Planner, lint, transport, decode, `Dispatcher::onRecord`,
the gma fast path, sharding. Plus chart fill, which spans **embassy**, **treaty** and
**customer-layer** and is therefore its own set of tickets. Repos: GMA_V3, embassy,
treaty, customer-layer.

**P3 — Event time.** `StreamValue.eventTimeNs`, event-time drivers for the window nodes,
the `bucketStartMs` SPEC amendment, warm-up gate, replay parity. Repo: GMA_V3, with a
treaty/wire touch for Q1.

P3 is **upstream of almost everything** — nothing in P1 or P2 is verifiable without it —
and it is the one most likely to be under-estimated because it looks like a field
addition. Order: P3 → P1 → P2, overlapping where the dependency allows.

### Ticket breakdown sketch

Dependency order, one repo each, ≤5h each. Not created — several depend on §16.

| # | ticket | project | depends on |
|---|---|---|---|
| 1 | `[GMA_V3] benchmark harness: ClickHouse → gma transport (B1)` | P2 | Q2 |
| 2 | `[GMA_V3] benchmark harness: rows/sec through the DAG (B2)` | P2 | — |
| 3 | `[GMA_V3] StreamValue carries event time; nodes copy it through` | P3 | Q1 |
| 4 | `[GMA_V3] event-time driver for TumblingWindow` | P3 | 3, Q13 |
| 5 | `[GMA_V3] event-time driver for BucketTime` | P3 | 3, Q13 |
| 6 | `[GMA_V3] warm-up emission gate` | P3 | Q3 |
| 7 | `[GMA_V3] gma_data skeleton + L1 schema + ClickHouse DDL` | P1 | Q4, Q6 |
| 8 | `[GMA_V3] Databento DBN adapter` | P1 | 7, Q14 |
| 9 | `[GMA_V3] restartable bulk ingest + completeness table` | P1 | 7, 8 |
| 10 | `[GMA_V3] Dispatcher::onRecord — non-JSON ingress` | P2 | — |
| 11 | `[GMA_V3] shim: planner (manifest → lossless SELECT, warm-up depth)` | P2 | 7 |
| 12 | `[GMA_V3] shim: SQL lint + corpus gate + WILL_FAIL fixture` | P2 | 11 |
| 13 | `[GMA_V3] shim: transport + decode + symbol interning` | P2 | 11, 1 |
| 14 | `[GMA_V3] ReplaySource over the shim, as an ingress kind` | P1 | 10, 13 |
| 15 | `[GMA_V3] inline DAG drive (no pool hop on the fast path)` | P2 | 2, 10 |
| 16 | `[GMA_V3] shard by (symbol, day) + cross-symbol refusal` | P2 | 14, Q7 |
| 17 | `[GMA_V3] SinkNode + RecordingSink` | P2 | 7 |
| 18 | `[GMA_V3] replay-parity fixture + CI gate` | P3 | 14, 17 |
| 19 | `[GMA_V3] WS query request type + lossless response frames` | P2 | 14 |
| 20 | `[embassy] seed baseMs from the fill window; pre-size createBuffer` | P2 | 19, Q10 |
| 21 | `[GMA_V3] SplicedSource state machine` | P1 | 14, 6 |
