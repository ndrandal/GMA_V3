# GMA_V3 Data Layer — ClickHouse selects, gma computes

**Status:** design, revision 3. Nothing implemented. ENC-1478.
**Locked decisions + ticket breakdown:** `specs/2026-10-14-gma-data-layer/SPEC.md`.
**Read §1 first.** It is one decision and everything else is downstream of it.

---

## 0. TL;DR

Raw Databento **trades + mbo** (L3, per-order) for **one symbol over seven days** lands in
a single-node, self-hosted ClickHouse **L1** store. There are no derived layers. ClickHouse
is indexed, compressed, columnar **storage and selection only** — it never computes a value
gma could compute. A **shim** reads a gma manifest, derives the minimal lossless `SELECT`
that satisfies it, and streams column blocks into gma with no row materialization. gma
performs **100% of the computation**, through the same operator code whether it is live,
replaying, filling a chart, or running headless research.

That boundary is a product requirement, not a preference. A transpiler that reimplemented
gma's math in SQL would produce small numerical differences — float accumulation order,
bucket edge inclusivity, EMA seeding, gap handling — and this system's business is reading
differences of that size as signal. There is no tolerance at which that is safe.

**The first deliverable is a measurement, not a feature.** At one symbol and seven days
almost everything built for scale is premature, and the one number the rest of the design
depends on — whether gma can consume rows as fast as ClickHouse delivers them — is cheaply
measurable on real data today. §14 is therefore the front of the ticket order, not the back.

---

## 1. The one decision

> **ClickHouse does selection. gma does computation. Nothing computes a value twice.**

### 1.1 Why a transpiler is not an option

Compile a gma graph to SQL and let ClickHouse execute it, and the chart's **filled** region
and its **live** region were produced by two different implementations. Even a flawless
translation differs: float32/float64 accumulation and summation order, window-boundary
inclusivity, EMA/RSI seeding over the first N samples, NaN and empty-bucket handling,
tie-breaking among records sharing a timestamp.

None of those is a bug. All of them put a kink at the seam, and **this system exists to
notice differences of exactly that size and call them an indication.** A discrepancy
manufactured by the transport layer is indistinguishable from signal.

A diff-based parity gate does not rescue it: two engines will not agree bit-for-bit on
floating-point aggregation, so such a gate must carry a tolerance, and the tolerance is
precisely the noise floor we cannot accept.

**So there is one implementation of every operator, and it is gma's.**

### 1.2 Why selection is safe

Selection is lossless *by definition* — a row either satisfies a predicate or it does not,
and no arithmetic happens on the way.

| permitted | why it is lossless |
|---|---|
| `WHERE` on `symbol` / `ts_event` / `provider_seq` | set membership |
| partition pruning (`toYYYYMMDD(ts_event)`) | skips parts |
| column projection | drops columns the manifest never reads |
| `ORDER BY` matching the physical primary key | no sort runs; the order is on disk |
| `LIMIT` **at the query-layer boundary only** | a row cap on a raw page — hazard below |

| forbidden, without exception |
|---|
| any aggregate function (`avg`, `sum`, `min`, `max`, `count`, `argMin`, …) |
| any window function (`OVER`, `LAST_VALUE`, `exponentialMovingAverage`, …) |
| any arithmetic or expression in the projection list |
| `GROUP BY` of any kind |
| time bucketing (`toStartOfMinute`, `intDiv(ts, N)`, …) |
| `MATERIALIZED` / `ALIAS` columns that compute a value |

**`LIMIT` hazard.** Safe as a page cap on a raw row request. **Not** safe as a bound on a
graph's input: it truncates the warm-up region and changes every value the graph emits. The
planner may emit `LIMIT` only for a raw page, never for a computation feed.

### 1.3 How the boundary is enforced

By a **lint** on generated SQL (§5.4), not a diff: parse each statement and reject any
construct from the forbidden list. That fails at build time on the *shape* of a query
rather than detecting a numerical divergence after it shipped, and it is strictly stronger
than any parity tolerance.

---

## 2. What exists today

`master` @ `90f0778`.

### 2.1 Ingest, and why the book half is nearly free

```
TCP :9001 ──▶ FeedServer::handleLine ──┐
                                        ├──▶ Event{symbol, shared_ptr<rapidjson::Document>, type}
wss:// ──▶ WsFeedClient ──▶ IFeedAdapter│         │
            ::translate() ──▶ FeedEvent ┘         ▼
                   │                      Dispatcher::onTick
                   └──▶ OrderBookManager (book mutations, seq/epoch)
```

- **`engine::IIngressSource`** (`include/gma/engine/IngressRegistry.hpp`) — `start()` /
  `stop() noexcept`, factories on `IngressRegistry` instantiated from `cfg.ingress[]` by
  the composition root (ENC-31). The right seam; reused as-is.
- **`ItchAdapter` already routes the MBO vocabulary.** `connectors/market/src/feed/
  ItchAdapter.cpp:29-40` dispatches `add_order`, `add_order_mpid`, `order_executed`,
  `order_cancel`, `order_delete`, `order_replace`, `trade`, `system_event`,
  `stock_directory` — i.e. add / cancel / modify / clear / trade / fill — onto
  `OrderBookManager`'s per-order API, with `tests/ob` and `tests/book` behind it. **The
  Databento MBO adapter is therefore a re-encoding of an existing tested mapping (binary
  DBN for ITCH JSON), not new book semantics.**
- **`gma::feed::FeedEvent`** already carries `orderId`, `timestampNs` and
  `optional<uint64_t> seq` on the OB variants. The §8 schema derives from it.
- **`OrderBookManager`** has the L3 machinery: `onSeq` gap gating, per-symbol
  `FeedState{lastSeq, epoch, stale}`, `onReset(symbol, epoch)` with an epoch'd `OrderKey`,
  `requestSnapshotFn`, and `onSnapshotPerOrder(symbol, orders, snapshotSeq)` taking a
  resume seq. Prices are `Price{int64_t ticks}` internally; public entry points are
  `double`.
- **`ObTradeEvent::bookNeutral`** exists because an execution both mutates the book *and*
  prints — "consuming the book would double-count". That precedent is directly relevant to
  §3/C13.
- **`gma::Event`** is a JSON DOM. `Dispatcher::onTick` iterates
  `payload->MemberBegin()..MemberEnd()` per tick (`src/core/Dispatcher.cpp:138`);
  `MarketTickComputer::compute` does `HasMember` probes (`MarketTA.cpp:342-367`).

### 2.2 DAG execution, and where it spends its time

- **`StreamValue{std::string symbol, ArgType value, int64_t bucketStartMs}`** — note the
  `std::string`: a symbol is **heap-copied into every value on every edge**.
- **`INode::onValue(const StreamValue&)`** — one scalar, one virtual call, per node, per
  value.
- **`Dispatcher::deliver`** posts to `rt::ThreadPool` for every node without a strand;
  ENC-1005 gives each request DAG an `rt::Strand` and calls strand-bearing `Listener`s
  inline.
- **`AtomicStore`** — latest-value-wins `(symbol, field)` behind a `shared_mutex`, written
  per derived value. ENC-1340 bounded reader starvation at one yield.
- **Clocking is wall-clock.** `BucketTime` / `TumblingWindow` use `system_clock::now()` +
  `wait_until` (`BucketTime.cpp:70`, `TumblingWindow.cpp:76`); `Interval` uses
  `cv.wait_for` (`Interval.cpp:33`). Each owns a thread that fires whether or not data
  arrived.
- **Pull sampling.** ENC-1335 `samplesAtClock()`/`bindAtClock()` — an `AtomicAccessor`
  *adjacent to a clock hop* samples on the ingress thread; deeper placements keep
  execution-time sampling by design.
- **Egress is lossy.** `update` frames coalesce above 256 queued and displace the oldest
  pending `update` at 4096 (ENC-996 / ENC-1072).

### 2.3 What does not exist

No Arrow, ClickHouse, Parquet or DuckDB anywhere (`third_party/` holds only `rapidjson`).
No history, persistence, replay or backfill. Event timestamps are extracted
(`MarketTA.cpp:364`) and stored as a **string** atomic (`:392`), read by nothing.

### 2.4 Downstream constraints that bind this design

- **embassy owns the chart time axis.** `TimeBasis{baseMs, periodMs, epochKnown}` rides
  `createBuffer`; records carry a **bar ordinal**, `ordinal = (bucketStartMs - baseMs) /
  periodMs` (`embassy/internal/pipeline/orchestrator.go:212`).
- **`baseMs` is frozen; ordinals are unsigned.** `orchestrator.go:787` is
  `baseMs.CompareAndSwap(0, bucketStartMs)` — the first bucket ever seen. Frozen
  deliberately: embassy gates scene-init rebroadcast on a byte compare, so a changing
  `baseMs` re-runs `createBuffer` and **zeroes every connected client's buffer** (ENC-1101).
  With `ordinal uint32`, **history before `baseMs` is not representable** (§10).
- **DynaCharting does not speak Arrow.** It consumes packed `float32` via `PackAppend`.
  Arrow stops at gma/embassy.
- **No chart-fill path exists.** `customer-layer/src/api/history.ts` is *pipeline build*
  history (ENC-825), not market data.

---

## 3. Where this conflicts with what exists

### C1 — Event time does not exist, and the clock nodes are wall-clock threads  **(blocking, post-measurement)**

`StreamValue` has no event-time field; `bucketStartMs` is wall-clock-derived. Replay a
10-minute window through `TumblingWindow(60s)` and you get **one** bar — a different
*number* of outputs, so no tolerance recovers it.

1. `StreamValue` gains `int64_t eventTimeNs` (an aggregate with default member
   initializers, so every `StreamValue{sym, val}` site still compiles — ENC-1280's
   property).
2. Every value-transforming node copies it through, as they do `bucketStartMs`.
3. `TumblingWindow` / `BucketTime` get an **event-time driver**: a boundary is crossed when
   the watermark passes it, not when a thread wakes. The wall-clock thread survives in live
   mode only, as the "close a quiet bar" path, and is disabled in replay.
4. `bucketStartMs` is re-derived from event time — **wire-visible** against
   `specs/2026-09-20-timestamps-on-the-wire/SPEC.md` D9/D10. → **Q1**.

**Not in the first slice.** B1/B2 (§14) use a non-windowed graph and do not need it, which
is why measurement goes first.

### C2 — The engine's ingress unit is a JSON DOM  **(blocking, in the first slice)**

```cpp
void onTick(const Event& tick);                 // existing JSON path, unchanged
void onRecord(const data::TickRecord& rec);     // new: resolved fields, no DOM
```

The JSON path stays for the legacy TCP / `market.wsclient` ingress. Two ingress shapes is a
cost; folding them is a bigger one. This is **not** a question about Arrow's reach — Arrow
is the shim's currency and stops at its decode; gma's operators stay scalar because §1
forbids a second implementation and rewriting them *is* one.

### C3 — No sequence number on the engine value path, and none on `TickEvent`

`feed::TickEvent` has `timestampNs` and **no `seq`**. `provider_seq` is mandatory and
non-optional in the schema. Databento supplies it, so the synthesis path (§9.4) is for
feed-simulator and future providers only.

### C4 — Warm-up and replay are different modes — **resolved by the planner**

`MarketTickComputer::_symbolHistories` and `Dispatcher::_histories` are populated only by
`onTick`; there is no way to advance state without firing every listener.

The planner already computes `effective = requested − warmup` (§5.1), so the emission rule
is one line and needs no mode flag:

> **emit iff `ts_event >= requested.fromNs`**

Live warm-up suppresses its prefix; research emits its window; chart fill emits the filled
range and suppresses the warm-up before it. (Formerly Q3.)

### C5 — Output capture cannot be at the WebSocket boundary

The outbox drops `update` frames by design above 256 queued, and
`docs/ARCHITECTURE.md` §8 names *event replay* as the case it breaks. Research output and
parity recording are captured at the `Responder`'s send function, upstream of the outbox
(§11).

### C6 — gma's hot path is transport-bound, and with MBO that is the whole cost  **(the measurement)**

Per value today: a `std::string` symbol copy, a virtual call per node, a
`ThreadPool::post` hop for every non-strand node, an `AtomicStore` write under a
`shared_mutex`, and a rapidjson DOM at ingress. For a chart tick, irrelevant. **For MBO —
tens of millions of records per symbol-day — it is the dominant cost, not the math.**
§6 is the plan and it touches no operator.

### C7 — Deterministic execution needs an explicit quiesce, not a sleep

`Dispatcher.hpp` records single-threaded ingress as a *precondition* of ENC-1005's ordering,
"one `for (...) threads.emplace_back([&]{ioc.run();})`" from breaking. Replay and research
runs assert it, set `threadPoolSize = 1`, and end on `ThreadPool::drain()` — the
deterministic barrier ENC-1340 established; a longer sleep is "not a fix, it is a wider
window". Inline DAG drive (§6) makes this cheaper: no pool to drain on the fast path.

### C8 — `IIngressSource` has no completion signal and no clock

`start()`/`stop()` is right for an endless feed, wrong for a replay that *finishes*. Both
additions live on a `gma_data` interface adapting *to* `IIngressSource`, so wiring stays an
INI edit.

### C9 — Fixed-point scale — **resolved: global 1e-9**

DBN prices are `int64` at 1e-9. The schema uses a **single global 1e-9 scale** — no per-row
scale column, no per-symbol lookup on decode, an exact integer path from DBN with no double
round trip. `OrderBookManager`'s per-symbol tick stays a book-quantization concern; its
remaining `int64 → double` wants `Price`-typed overloads eventually. (Formerly Q4.)

### C10 — ClickHouse is a required service — **resolved: two deployment profiles**

| profile | store | footprint |
|---|---|---|
| **research / ingest** (your local machine) | single-node self-hosted ClickHouse L1 | a required service, by design |
| **client deployment** | live-only, or reads a central L1 over the wire | gma + a wire client; no local store |

Both are `IL1Store` configurations plus two build profiles. This also resolves the
sovereignty-vs-central-hosting contradiction into a profile choice. **And the dependency
budget improves: no Parquet store means no DuckDB**, the largest single dependency, which
existed only to query Parquet (§15).

### C11 — `AtomicStore` is mutable latest-value state, and deep pull nodes race it

ENC-1335 binds the sample to the clock **only at the two adjacency points**; a pull node
deeper in a chain keeps execution-time sampling by design, so such a graph is not
bit-reproducible. Existing documented behaviour, not introduced here. → **Q5**.

### C12 — ~~`ReplacingMergeTree` dedup is only exact on a unique key~~ — **dissolved**

Revision 2 proposed `ReplacingMergeTree` for idempotent re-ingest, which was only exact
where the sort key is unique — and for `book_events` it is not, so a background merge could
silently delete a real row in a way indistinguishable from a gap.

**Resolved by changing the re-ingest mechanism instead of the key** (§8.6): plain
`MergeTree`, with `gma.ingest_windows` as the authority and re-ingest by `DROP PARTITION` +
reload from the retained DBN files. No dedup means nothing can collapse two distinct MBO
records, so **the sort key no longer needs to be unique**. Whether `action, order_id` joins
the key is now a granule-skipping question, measured in T6, not a correctness gate.

This is why **retaining the raw DBN files is load-bearing** (§8.7), not hygiene: reload has
to reload *from something*, and re-pulling from Databento spends the plan allowance this
whole architecture exists to work around.

### C13 — trades + MBO puts every execution in the store twice  **(new)**

Each execution appears as a `trades` row **and** as MBO trade/fill records. Not duplicates
in the bug sense — two views — but a graph reading both **double-counts volume**.

The repo already knows this shape: `ObTradeEvent::bookNeutral` exists because "executions
already mutate the book via an explicit add/update/delete… consuming the book would
double-count". That is the live-path guard; the store needs its equivalent, and the planner
is where it goes — it already derives which `kinds` a graph touches, so it must **refuse**
a graph that reads `trades` and `book_events` for the same symbol and window. Silently
summing both is a wrong answer with a completely plausible shape. Refuse by default;
reconciliation is a later, explicit opt-in. → **Q15**.

### C14 — `OrderBook::byId_` is uncapped  **(minor)**

`std::unordered_map<OrderKey, Locator, OrderKeyHash>` with no bound. Resting-order counts
self-bound in practice (cancels remove entries), but a gap-ridden or corrupt MBO stream can
grow it without limit, and `maxSymbols` does not cover it. Wants a cap.

---

## 4. Components and data flow

```
┌─ Databento historical: trades + mbo, 1 symbol, 7 days ──────────────────────┐
│   raw DBN files RETAINED on disk — L1 is a rebuildable projection of them    │
└──────────────────────────────┬──────────────────────────────────────────────┘
                               ▼
┌─ gma_data ──────────────────────────────────────────────────────────────────┐
│  IProviderAdapter             INormalizer                                   │
│  ┌──────────────┐             ┌─────────────────────────────────┐           │
│  │ Databento    │   raw DBN   │ decode → canonical columns       │           │
│  │ (trades+mbo) │ ──────────▶ │ (ts_event, provider_seq,         │           │
│  ├──────────────┤             │  session_id, symbol,             │           │
│  │ feed-sim     │             │  instrument_id, venue)           │           │
│  └──────────────┘             └───────────────┬─────────────────┘           │
│                                               │ symbol-sorted, per day       │
│                            ┌──────────────────▼──────────────────┐          │
│                            │  ClickHouse L1 — RAW ONLY           │          │
│                            │  single node, self-hosted, podman   │          │
│                            │  plain MergeTree, PK (symbol,ts,seq)│          │
│                            │  storage + selection. NO compute.   │          │
│                            └──────────────────┬──────────────────┘          │
│  ┌──────────────────────────────────────────── ▼ ──────────────────────────┐│
│  │ THE SHIM                                                               ││
│  │   planner  — manifest → minimal lossless SELECT; index-aware;          ││
│  │              warm-up depth DERIVED from the graph; refuses C13         ││
│  │   lint     — reject any generated SQL containing compute (§5.4)        ││
│  │   transport— native column blocks, zero-copy, back-pressured           ││
│  │   decode   — columns → POD, symbols INTERNED. ONE conversion point.    ││
│  └──────────────────────────────────────────┬─────────────────────────────┘│
│  ISource:  LiveSource │ ReplaySource │ SplicedSource (§9, demoted)          │
└─────────────────────────────────────────────┼──────────────────────────────┘
                                              ▼
┌─ gma_engine — ALL COMPUTATION, ONE IMPLEMENTATION ──────────────────────────┐
│  Dispatcher::onRecord ──▶ IEventComputer ──▶ AtomicStore                     │
│                       ├─▶ OrderBookManager (MBO per-order API)               │
│                       └─▶ Listener ──▶ … event-time clocked nodes … ──▶      │
│                                     Responder ──▶ WS (charts)                │
│                                     SinkNode  ──▶ Arrow table (research)     │
└─────────────────────────────────────────────────────────────────────────────┘
```

Six rules:

1. **ClickHouse computes nothing semantic.** Enforced by the lint, not a diff.
2. **One implementation of every operator**, in gma, shared by every consumer.
3. **gma owns no durability.** It reads L1; it never writes the raw path.
4. **One conversion point** — column blocks → POD, column-wise, in the shim's decode.
5. **The splice lives in `ISource`** and nowhere else.
6. **`gma_data` depends on `gma_engine`, never the reverse.** Arrow and the ClickHouse
   client must not become engine dependencies.

---

## 5. The shim

### 5.1 The planner

**It analyses the graph's data dependencies. It does not translate the graph's math.** That
distinction is the whole design.

Given a manifest (the same JSON node graph `TreeBuilder` consumes) plus a window:

| derived | from | why |
|---|---|---|
| symbols | the graph's `Listener` `streamKey`s | predicate on the PK's leading column |
| time range | requested **minus warm-up depth** | below |
| columns | the `field`s the graph reads, via `MarketFieldMap` | a column store charges per column; `SELECT *` discards the layout |
| record kinds | which of `trades` / `book_events` the graph touches | **and refuses both at once — C13** |
| ordering | always `(symbol, ts_event, provider_seq)` | matches the physical key, so no sort runs |
| shards | `(symbol, day)` tiles | §7 — deferred at current scale |

**Warm-up depth is derived, not configured.** `sma_200` over 1-minute buckets needs 200
extra minutes of input, or the first 200 outputs are wrong. The planner walks the graph for
the deepest history requirement across every operator and bucket period. **This is the only
place that number can be correct** — a human setting it per request gets it wrong silently,
and in the direction of too little, producing output that looks plausible and is warm-up
garbage. It subsumes a `lookbackNs` knob and it is what makes C4's one-line emission rule
work.

### 5.2 Transport

1. **Column blocks, not rows.** Native protocol speaks columns; HTTP speaks `FORMAT
   ArrowStream`. **Self-hosted on loopback makes native the expected answer** — no network,
   and HTTP framing is pure overhead — but it is **B1**, measured, not assumed.
2. **Zero-copy** into gma's address space where the wire format permits.
3. **Bounded buffering with back-pressure.** ClickHouse reads faster than gma computes;
   without a bound a long scan buffers it all in RAM. A stall here is *good news* — it
   means gma is the bottleneck, which is what B2 is trying to establish.
4. **Order preservation**: PK order within a shard; no multi-part merge that interleaves.
5. **Batch size tuned to cache**, not convenience.

### 5.3 Decode

One column-wise function producing `TickRecord` / `BookRecord`. Two details:

- **Symbols are interned to a `uint32` id.** `StreamValue` carries `std::string symbol`, so
  today a symbol is heap-copied into every value on every edge. At MBO rates that single
  copy is a measurable fraction of total cost.
- **No DOM is constructed anywhere.** This is `onRecord` (C2), and it is a net *reduction*
  in per-tick work versus the shipped JSON ingress.

**Where the two halves live — settled by ENC-1481.** The PODs are in
`include/gma/data/Record.hpp`, i.e. inside **`gma_engine`**, not under `data/`. C2's
`Dispatcher::onRecord(const data::TickRecord&)` is engine code and has to be able to name
the type, so a POD reachable only from `gma_data` would make C2 unimplementable without
breaking §4 rule 6. The PODs are Arrow-free and dependency-free; the `arrow::Schema`s and
this decode stay in `data/include/gma/data/Schema.hpp`, which is where Arrow stops. Same
namespace (`gma::data`), one definition per type — the library boundary is a *dependency*
boundary, not a naming one. `SymbolTable` sits beside the PODs for the same reason:
interning happens in decode, but resolving an id back to a ticker happens at an output
boundary, and every output boundary is engine code (`StreamValue` carries a `std::string`).
So the invariant to enforce is not "no `gma/data` header reaches the engine" but **"Arrow,
the ClickHouse client and `gma_data` are not engine dependencies"** —
`tools/check_engine_isolation.py`, ctest case `gma_engine_isolation`.

### 5.4 The lint — the structural guarantee

Every emitted `SELECT` is parsed and checked against §1.2's forbidden list. A violation is
a **build-time failure**, not a runtime warning:

```
gma_shim_sql_lint:  planner output for corpus graph 86 contains
                    aggregate function 'avg' in projection list
                    -> computation must not leave gma (docs/data-layer.md §1)
```

Run over `tests/treebuilder/corpus_requests.json` (272 entries), this asserts no manifest in
the corpus can push computation down.

**Anti-vacuity is part of the contract.** A lint that silently matched zero statements reads
exactly like a passing one — the failure mode ENC-1289's expected-failure machinery exists
to prevent. So: assert a non-zero statement count, and ship a deliberately-violating
fixture registered `WILL_FAIL`.

---

## 6. gma's fast path — transport, not semantics

The performance budget. **None of it touches an operator.** Each row stays bit-identical by
construction: the same function, called in the same order, on the same arithmetic. Only
what happens *between* calls changes.

| change | mechanism | semantic risk |
|---|---|---|
| no JSON DOM at ingress | `Dispatcher::onRecord` (C2) | none |
| no `ThreadPool` hop between nodes | drive the DAG **inline** per record on one thread | none — same calls, same order, and it is what C7 wants anyway |
| no symbol heap copy per value | intern to a `uint32` in decode (§5.3) | none |
| no `AtomicStore` write per derived value | write only what a pull node reads | none |
| amortized dispatch | batch the **drive loop**; operators unchanged | none |

Measure in that order (B2) and stop when gma beats the scan. **The first four need no node
changes at all.**

If that is not enough, **opt-in per-node batch paths** — a `Worker` consuming a contiguous
span instead of N scalars — gated by an **exact** equality assertion against the scalar path
over the corpus. Exact is achievable because it is the same arithmetic in the same order;
that is the property a SQL transpilation could never offer.

**Explicitly off the table:** reimplementing any operator's math in a vectorized form that
changes accumulation order. That is a second implementation wearing a different hat.

---

## 7. Scale: shard, don't transpile — deferred

One implementation means gma touches every row, and MBO at the full universe is billions.
The answer preserving identical semantics: **gma's semantics are already per-symbol**, so
partition by `(symbol, day)`, run N independent complete gma computations, concatenate. No
new code path, no merge semantics, nothing to diverge.

**Deferred at current scale** — one symbol × 7 days is 7 tiles and nothing to parallelize.
When it lands:

- The planner emits shard-aligned queries, each hitting the PK cleanly.
- **Cross-symbol operators must be detected, not trusted.** `Aggregate`/`JoinBy` with
  `by:"streamKey"` shards fine; `by:"none"` (ENC-1292) does not, and sharding it silently
  produces tuples pairing a symbol with itself. → **Q7**.
- Per-shard determinism is C7 applied per shard — stronger and simpler than making one
  multi-threaded run deterministic.

---

## 8. Canonical schema — L1 only

`data/include/gma/data/Schema.hpp`; DDL in `data/schema/clickhouse.sql`.

### 8.1 There are no derived layers

L1 holds raw Databento records. No L2, no rollup, no materialized bar table, no feature
store. Research is ad hoc with no stable access pattern, so a derived layer is a bet on
queries nobody has written — and under §1 any *computed* layer is a second implementation
besides.

Cost, recorded so the trade is explicit: a long-interval chart fill (1000 × `1d`) scans a
wide range of raw rows to return 1000 bars. **Softened by having `trades`:** the planner
picks the table from the graph's needs, so an OHLC fill reads `trades` and never MBO. →
**Q8**, after B3.

### 8.2 Projections are allowed, and are not a layer — deferred

Same table, same columns, no new semantics: a second physical sort order maintained on
insert. An index, not a model. `(ts_event, symbol)` serves cross-sectional research, which
the symbol-leading PK does not.

**Deferred: pointless at one symbol.** The DDL carries it commented, with the reasoning, so
it is a one-line enable when the universe grows.

### 8.3 Common columns

| column | type | notes |
|---|---|---|
| `ts_event` | `DateTime64(9,'UTC')` | venue event time. **The only time an operator may key off.** |
| `ts_recv` | `DateTime64(9,'UTC')` | normalizer receipt. Diagnostics only — never an operator input, never a window boundary. |
| `provider_seq` | `UInt64`, non-null | from Databento. Monotonic within `(session_id, symbol)`. |
| `session_id` | `UInt32` | ingest session. |
| `symbol` | `LowCardinality(String)` | **the ticker** — see §8.9. |
| `instrument_id` | `UInt32` | **the DBN id**, retained for provenance — §8.9. |
| `venue` | `LowCardinality(String)` | Nasdaq / OTC / … |

Prices are **`Int64` at a fixed global 1e-9 scale** (C9).

### 8.4 Tables in the first slice

- **`trades`** — `price`, `size`, `aggressor`, `book_neutral`, `trade_id`, `flags`.
- **`book_events`** (MBO / L3) — `action`, `side`, `price`, `size`, `order_id`, `priority`,
  `depth`, `epoch`, `flags`. `action` mirrors the `feed::FeedEvent` variant set one-for-one.
- **`book_snapshots`** — §8.5.
- **`symbology`** — §8.9.
- **`ingest_windows`** — §8.8.
- **`gaps`** — discontinuities as rows, not metrics. A research run that cannot see "there
  was no data here" treats a gap as a flat market: a wrong answer, not missing telemetry.

**`quotes` (L1 top-of-book) is NOT in the first slice.** With no `mbp-1` in the plan nothing
populates it — top-of-book is computed from the reconstructed book via
`OrderBookManager::bestBid`/`bestAsk`, which is correct under §1 and consistent with `ob.*`
being pipeline-only (ENC-94/101). The table stays defined for feed-simulator and future
L1-only providers.

### 8.5 `book_snapshots` — required, because MBO

With MBO, replaying an arbitrary window without a seed means replaying **every order event
from session open** — a full trading day of per-order messages, *per research run*. That is
the dominant cost of any windowed research, so snapshots are required rather than argued.
`onSnapshotPerOrder(symbol, orders, snapshotSeq)` already takes the resume seq.

A snapshot is materialized state, which brushes against §8.1; the distinction is that it is
raw book **state**, not an aggregate — `OrderBookManager` is the single implementation of
applying deltas either way. Cadence: session open plus a fixed event-time interval, tuned
against B3. (Formerly Q9, now settled in principle; cadence is a tuning parameter.)

### 8.6 Engine and re-ingest — plain `MergeTree`, drop-partition reload

```sql
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
```

**Plain `MergeTree`, not `ReplacingMergeTree`.** Re-pulling windows will happen — a gap, a
correction, a schema change — and the mechanism is `DROP PARTITION` + reload from the
retained DBN files, with `gma.ingest_windows` as the authority on what is loaded.

Why this and not dedup:

- **It dissolves C12.** No dedup means nothing can silently collapse two distinct MBO
  records, so the sort key need not be unique.
- **No merge amplification.** `ReplacingMergeTree` on an MBO-sized table rewrites enormous
  parts for a property we get more cheaply.
- Cost: partitions are day-granular, so re-pulling one symbol-day reloads that whole day.
  At one symbol that is identical. If per-symbol re-pulls become common, a lightweight
  `DELETE WHERE symbol = … AND ts_event IN range` then insert is the finer-grained form.

**Pick one or the other, never both:** a `ReplacingMergeTree` with a non-unique key
collapses rows no matter how you re-ingest.

Insert **symbol-sorted batches per day partition** — the key leads with symbol, so raw DBN
file order (time-ordered) forces a per-part re-sort. The ingest job owns this; a background
merge cannot fix it after the fact.

### 8.7 The retained DBN files are load-bearing

**L1 is a derived, rebuildable projection of the raw DBN files, and the files are kept.**
Not hygiene — §8.6's re-ingest mechanism depends on it, and three things follow:

- Re-ingest is **free** rather than costing Databento plan allowance, which is the
  constraint this architecture exists to work around.
- A schema change is a **replay**, not a re-purchase. At MBO scale the schema will change at
  least once.
- A single local disk with no replication and a `DROP PARTITION` that can be mistyped is
  otherwise one error from unrecoverable.

DBN ships zstd-compressed and is typically *smaller* than the ClickHouse table it produces,
so this is close to free. If the files are ever not retained, `ReplacingMergeTree` with a
provably-unique `book_events` key becomes mandatory and C12 returns.

### 8.8 `ingest_windows` — completeness

Three situations are **indistinguishable in a plain `SELECT`**, and the third silently
returns a flat market:

| cause | detection |
|---|---|
| the market was quiet | no seq discontinuity — this is data |
| the provider has a hole | seq discontinuity — a `gaps` row |
| **we have not ingested it yet** | not in `ingest_windows` — **not a gap** |

So completeness is **recorded by the ingest job, never inferred from row counts** (a quiet
window and a missing window have the same count, which is the whole problem), and
`IL1Store::isComplete` makes a query over an incomplete window **fail** rather than answer
short. It also makes ingest restartable: a ClickHouse insert is atomic per block, not per
batch, so an interrupted batch leaves some blocks committed and recovery is re-running the
window — which §8.6 makes exact.

### 8.9 Symbol identity — ticker as the key, `instrument_id` for provenance

DBN records carry `instrument_id`, which is dataset-scoped and reassignable; humans, charts
and manifests all refer to tickers; and gma's `streamKey` is an opaque string that wants to
be `"AAPL"`.

**Decision: `symbol` = `raw_symbol` (the ticker) is the primary key's leading column.
`instrument_id` is retained as a column. A `gma.symbology` table holds the time-varying
mapping.**

- Keying L1 on `instrument_id` would make every query require a symbology join and the
  store unreadable by hand.
- `instrument_id` must be kept regardless: it is what the records contain and how a row is
  traced back to the pull that produced it.
- **The mapping table goes in now, at one symbol, precisely because it costs nothing now and
  it is the thing that cannot be retrofitted.** Once there are months of rows keyed on a
  bare ticker with no provenance, reconstructing what a ticker meant on a given day is
  archaeology.

```sql
CREATE TABLE IF NOT EXISTS gma.symbology
( dataset LowCardinality(String), instrument_id UInt32,
  raw_symbol LowCardinality(String),
  valid_from DateTime64(9,'UTC'), valid_to DateTime64(9,'UTC'),  -- half-open
  source_ref String )
ENGINE = ReplacingMergeTree ORDER BY (dataset, instrument_id, valid_from);
```

(`ReplacingMergeTree` is safe *here*: the key is unique by construction and the table is
tiny.)

**Not solved, and named rather than hidden:** a ticker delisted and later reassigned to a
different company. `valid_from`/`valid_to` lets you **detect** it; what a continuous series
should do about it is reference data and out of scope.

**~~Unverified~~ — half of it is now measured (T4, ENC-1482).** The exact DBN field names
and the whole record/metadata layout were established against real vendor DBN bytes committed
at `data/tests/fixtures/dbn/`; they are written down in `dbn::` in
`data/include/gma/data/DatabentoAdapter.hpp` and in SPEC §5 under Q14, and the names there are
the vendor's own spellings — `instrument_id` included, so this section's field name is right.
**Whether `instrument_id` is stable within a dataset-day is still open**: a two-record public
sample naming one instrument proves nothing about reassignment, so **T5 confirms that against
a real 7-day file.**

### 8.10 Codecs

`DoubleDelta` on `ts_event` / `ts_recv` / `provider_seq`; `Delta(8), ZSTD(3)` on prices;
`T64, ZSTD(1)` on sizes; `Delta(8), ZSTD(1)` on `order_id` and `priority`. MBO prices are
highly repetitive (many orders at one price) and `order_id` is the high-cardinality column
that will dominate the table — worth confirming against real data in T5.

---

## 9. The live/historical splice — designed, demoted

**Demoted.** Databento Plus is unaffordable, so there is no live Databento feed, so nothing
in the research or chart-fill path splices. Only feed-simulator exercises it. It stays
designed because chart-attach-then-live needs it and live arrives when the plan tier does,
but it is **not in the first slice**. Named risk: it will ship exercised only against a
simulator.

```
BUFFERING ──▶ BACKFILLING ──▶ REPLAYING ──▶ DRAINING ──▶ LIVE
    │                                                     ▲
    └──────────────── gap / disconnect ───────────────────┘
                              └──▶ RESYNC ──▶ BACKFILLING
```

- **Subscribe before you query.** `BUFFERING` opens the live subscription *first* and
  records the first live `(seq, ts)` per symbol as the splice point. Query first and the
  window between "history ends" and "live begins" is lost with no error anywhere.
- **Dedup on the pair** `(session_id, provider_seq)`. A restart resets a synthesized seq to
  0, so bare-seq comparison silently re-admits replayed rows.
- **Per-symbol state, not per-source.** A source-wide machine stalls every symbol behind the
  slowest, which on an OTC feed may not print for an hour.
- **Buffer overflow aborts and retries** with a later `T`. Drop-oldest and drop-newest both
  punch a silent hole. Each abort increments a counter an operator can see.
- **L2/L3 gap recovery is already built.** `onSeq` gates each delta and returns false on a
  gap, marks stale, invokes `requestSnapshotFn`; the data layer supplies that callback —
  read the nearest snapshot from L1 (replay) or request one from the provider (live).
  `onReset` bumps the epoch `OrderKey` carries. **Unverified:** whether `ob::Provider`
  consults `isStale` before serving `ob.*`.

---

## 10. Chart fill — post-measurement

Same query layer, one binding constraint.

Per §2.4, `baseMs` is frozen and ordinals are `uint32` and only move up, so **history before
`baseMs` is not representable.** The fill depth must be known at scene-init, with `baseMs`
seeded from the start of the filled window rather than the first live bar — a one-line
change at embassy's CAS — and `createBuffer` pre-sized to history + live headroom, which
the wire already supports (`SceneBuilder.ts:457`).

**Consequence: infinite scroll-back is not expressible on the current wire.**
Attach-with-N-bars is. → **Q10**.

A chart is not "OHLC for AAPL": forum/aibox compiles to a scene whose slots bind gma
`requestId`s, so filling `candles-with-overlays-v1` means filling seven series, each a
pipeline's output, **each computed by gma from L1 through the same operators the live stream
uses.** That is the entire reason the transpiler was rejected.

The wire needs a **`query` request type** (`ClientSession` handles only `subscribe`/`cancel`)
whose response is a **lossless** frame class — a 50k-bar fill through the coalescing outbox
is exactly the ENC-996 failure. Arrow stops at embassy; the Arrow → `float32` pack is a real
server-side step.

Chart attach is the splice at the client boundary: fill to `T`, live from `T`, dedup at the
join. Different code on the two sides produces a doubled or missing bar — the classic
charting bug, and also a false indication.

---

## 11. Sinks and research output

A `SinkNode` is an ordinary `INode` registered as `"Sink"`, so it is specifiable from a JSON
graph exactly like a `Responder` and `TreeBuilder` needs no special case. It accumulates
emitted `StreamValue`s into an Arrow builder and flushes on a row-count or **event-time**
interval trigger — a wall-clock flush would produce different batch boundaries under replay
than live for identical input, the same defect class as a wall-clock window one layer out.

- **Research output** — the headless path: `ReplaySource` over L1 → DAG → `SinkNode` →
  Arrow table. "gma as the query engine transforming tables." The primary use.
- **`RecordingSink`** — fixed schema `(request_key, symbol, event_time_ns, bucket_start_ms,
  value)`, installed at the `Responder`'s send function, upstream of the lossy outbox (C5).
  `value` is **text, not a float column**: `ArgType` is a variant over
  bool/int/double/string/vector/`Record`, the corpus has `Record` terminals, and a float
  column would silently make an exact comparison approximate.
- **Derived-series persistence — optional cache.** Under §1 the recompute path is the same
  code, so a persisted copy is a cache, not a correctness requirement. If added, invalidate
  by **graph identity**: a persisted `sma20` from a changed manifest is a wrong answer with
  a plausible shape. → **Q11**.

Raw durability is never a sink's job.

---

## 12. Failure modes

| mode | handling |
|---|---|
| re-pull / correction | `DROP PARTITION` + reload from retained DBN; `ingest_windows` is the authority (§8.6, §8.7) |
| partial insert | atomic per block, not per batch. Re-run the window; §8.6 makes it exact. Requires restartability from a window boundary |
| quiet vs hole vs not-ingested | §8.8. The third must **fail**, not answer short |
| MBO sequence gap | `onSeq` returns false, marks stale, `requestSnapshotFn` re-snapshots (§9) |
| out-of-order within a symbol | should not happen. Count, log, pass through in arrival order — reordering hides the defect and adds latency |
| out-of-order across a splice | exact seq-based drop in `DRAINING`, not a reorder buffer |
| gma restart | holds no durable state; re-runs from L1 |
| ClickHouse unavailable | research and fill **fail loudly**. A live-only client profile keeps streaming and reports "no history", never a thin window |
| local disk loss | L1 is rebuildable from the retained DBN files (§8.7). Without them, it is not |

---

## 13. In scope / deferred

**First slice (the measurement).** ClickHouse via `tools/stack.sh`; L1 DDL;
`gma_data` skeleton + schema; Databento trades+mbo adapter; the 7-day/1-symbol ingest with
retained DBN and completeness rows; the sort-key uniqueness measurement; `onRecord`; B1 and
B2. §17 has the tickets.

**After the measurement, driven by it.** The shim's planner and lint; event time (C1) and
the window drivers; `ReplaySource`; `SinkNode` / `RecordingSink`; the §6 transport changes
B2 justifies; replay parity.

**Deferred, named.** Any derived layer (§8.1). Projections (§8.2 — pointless at one symbol).
Sharding (§7). Per-node batch fast paths (§6 phase 2). `SplicedSource` live half (§9). Live
TotalView / Databento Plus. `quotes` population (§8.4). Chart fill (§10). Multi-provider
merge for one symbol. Corporate actions / adjusted prices / symbol-change continuity.
Retention and compaction. `Price`-typed `OrderBookManager` overloads (C9). Infinite
scroll-back (§10). Parquet and DuckDB **entirely** (C10, §15).

---

## 14. Measurement first

**Nothing in §6 or §5.2 is chosen without these, and `GMA_V3/benchmarks/` has no query-path
target today. No number in this document is measured.**

| id | measures | notes |
|---|---|---|
| **B1** | rows/sec ClickHouse → gma address space, **transport only, no compute**; native vs HTTP `ArrowStream` | sets the ceiling; settles §5.2 |
| **B2** | rows/sec **through the DAG**: today's JSON path vs `onRecord` vs inline drive vs interned symbols | the §6 budget, change by change |
| **B3** | chart-fill p50/p99 at 1000 bars from `trades`, `1m`/`1h`/`1d`, cold and warm | post-measurement; informs Q8 |
| **B4** | shard scaling: throughput vs cores | deferred with §7 |

**The target is explicit: `B2 ≥ B1`.** If gma computes slower than ClickHouse delivers, gma
is the bottleneck and that is where the work goes. Under §1 we are not competing with SQL —
we are using it as an indexed disk — so this is the well-defined form of "as fast as SQL".

**Confound to design around, not discover: gma and ClickHouse share one machine.** They
compete for cores and page cache, so `B2 ≥ B1` can be satisfied for the wrong reason. B1/B2
therefore need pinned cores or a sequential protocol, and ClickHouse's
`max_server_memory_usage` must be capped so it cannot take the page cache the scan depends
on.

**B1/B2 do not need event time.** A raw-scan benchmark uses a non-windowed graph, which is
why measurement precedes C1's engine work.

### 14.1 Structural guarantees (replacing a parity tolerance)

- **`gma_shim_sql_lint`** (§5.4) — no generated `SELECT` over the 272-entry corpus contains
  computation. Non-zero statement count asserted; a deliberately-violating `WILL_FAIL`
  fixture ships with it.
- **Exact equality for any batch fast path** (§6) — bit-identical to the scalar path over
  the corpus. Exact, not approximate.

### 14.2 Replay parity — after event time

Proves **live output equals replay output** through one implementation, i.e. that event time
and the source layer are correct. Record once from **feed-simulator** — deterministic,
redistributable, and **not** Databento, because Databento data is licensed and committing it
to the repo is a redistribution question not worth answering. Build the graphs through the
**ordinary `TreeBuilder`**, drive `ReplaySource` at `threadPoolSize = 1`, drain, diff on
`(request_key, symbol, event_time_ns, bucket_start_ms, value)`. Byte-identical, no
tolerance.

Gates: **anti-vacuity** (`expected` non-empty, spans >1 bucket, contains windowed output),
and a **falsification case** `gma_replay_parity_wallclock` registered `WILL_FAIL` that runs
with the event-time driver disabled and **asserts the mutation applied** — a no-op mutation
reads exactly like a passing gate.

Known non-determinism to pin first: deep pull nodes (C11), `Interval`-clocked graphs
(**resolved: `Interval` is live-only and excluded from replay and research** — it is a
wall-clock cadence by construction; formerly Q13), and multi-symbol graphs where
`_listeners`' outer `unordered_map` ordering surfaces.

---

## 15. Dependency budget

**Estimates, except where §15.1 has measured them on this toolchain.**

| dep | why | verdict |
|---|---|---|
| Arrow C++ | the shim's currency and the research output type | required; trim with `ARROW_*=OFF`. **Measured — §15.1** |
| ClickHouse client (`clickhouse-cpp` **or** HTTP) | the only transport to L1 | required; which is **B1** |
| Parquet | — | **dropped** |
| DuckDB | — | **dropped.** Largest dependency; existed only to query Parquet |
| Databento client (`databento-cpp`) | avoidable by decoding DBN directly | **dropped — Q14 resolved by ENC-1482.** See §15.2 |
| zstd | `.dbn.zst` decompression | **not in `gma_data`** — the ingest job decompresses; feeding a zstd frame to the adapter is refused by name |

D14's client profile has two variants and they do not cost the same. The **over-the-wire**
variant (C10) needs Arrow plus a wire client and no store — far smaller than
Arrow+Parquet+DuckDB. The **live-only** variant needs no Arrow at all, which is the whole
premise of `GMA_BUILD_DATA=OFF`. `gma_data` is separately linkable so that one omits it,
and §15.1's Release row sizes the first, not the second.

### 15.1 Arrow, measured — ENC-1481, 2026-10-14

The line above said *estimates; measure on this toolchain*, and the ticket that built the
target budgeted **45 minutes** for the source build with an instruction to escalate rather
than grind if it ran over. **It takes 45 seconds.** The estimate was wrong by a factor of
about sixty, which is worth recording in the direction it fell: nothing in this design is
gated on Arrow's build cost, and no sharding, vendoring or caching work needs to be done to
make it tolerable.

Box: Fedora 44, 16 cores, GCC 16.2.1, CMake 4.3.0, Ninja. Arrow **23.0.1** from
`archive.apache.org`, `SOURCE_SUBDIR cpp`, trimmed by
`GMA_V3/data/cmake/GmaDataArrow.cmake`.

| phase | wall clock | artifact |
|---|---|---|
| download `apache-arrow-23.0.1.tar.gz` | **2m 33s** | 17.3 MB (`sha256 bd09adb4…de6b18`) |
| configure, cold (extract + Arrow's own configure) | **3.1 s** | — |
| configure, warm | 0.6 s | — |
| build, **Debug**, `-j16` | **45.0 s** | `libarrow.a` **366 MB**, bundled deps 7.2 MB |
| build, **Release**, `-j16` | **1m 02.7 s** | `libarrow.a` **33.0 MB**, bundled deps 1.4 MB |
| `gma_data` itself (Debug) | ~2 s | `libgma_data.a` 3.4 MB |

The download dominates, and it is a one-time cost per build tree; a warm tree pays none of
it. Debug `libarrow.a` is an order of magnitude larger than Release because it is `-O0`
with full debug info — size the over-the-wire client variant off the Release row (the
live-only variant links no Arrow at all).

**Who pays it: `GMA_BUILD_DATA` defaults to `GMA_BUILD_TESTS`.** `-DGMA_BUILD_TESTS=ON` —
the acceptance command and every gate configuration — builds `gma_data` and runs both
engine-isolation cases plus `gma_data_tests`, so nothing is silently skipped where tests
run. A bare `cmake -B build -DCMAKE_BUILD_TYPE=Release` fetches **nothing** and still
builds `gma_server`, which is what the documented quickstart needs offline (before this
library existed, a default configure fetched nothing at all — googletest sits behind
`GMA_BUILD_TESTS=OFF` too — and a fixed `GMA_BUILD_DATA=ON` had made that configure
hard-fail at the download behind a firewall). An explicit `-DGMA_BUILD_DATA=ON|OFF` always
wins, and the configure prints which way it resolved and why.

**What is off, and which entries are decisions rather than tweaks.** Arrow 23 already
defaults most of this off, but every one is set explicitly, because a default is something
a future release is free to change and a decision is not:

- `ARROW_PARQUET=OFF` and no DuckDB — **D14. Dropped from the design**, not trimmed.
- `ARROW_COMPUTE=OFF`, `ARROW_ACERO=OFF`, `ARROW_SUBSTRAIT=OFF`, `ARROW_GANDIVA=OFF` —
  **D1.** A kernel, a query plan or an expression JIT evaluating a value is a second
  implementation of an operator. This is the group most likely to be switched back on
  "for convenience".
- `ARROW_IPC=ON` — the only non-core module kept, and only because **B1** (ENC-1485)
  measures ClickHouse `FORMAT ArrowStream` over HTTP, which is Arrow IPC on the wire.
- `ARROW_WITH_LZ4=ON`, `ARROW_WITH_ZSTD=ON` — **not a size tweak either.** ClickHouse's
  `output_format_arrow_compression_method` defaults to `lz4_frame`, so a codec-free Arrow
  cannot read ENC-1485's stream at all without a per-query `SETTINGS` override. Both are
  bundled builds (lz4 ≈ 1 MB, zstd ≈ 2.3 MB of source); `-DGMA_DATA_ARROW_IPC_CODECS=OFF`
  drops them. Brotli, bz2, snappy and zlib stay off.
- Off and uncontroversial: shared libs, jemalloc, mimalloc, filesystem, S3/GCS/Azure/HDFS,
  Flight and Flight SQL, ORC, CSV and JSON readers, RE2, utf8proc, OpenTelemetry, UCX,
  testing, benchmarks, examples, integration and the CLI utilities.

**One honest caveat about `ARROW_COMPUTE=OFF`.** The build still compiles an
`arrow_compute_core` object group — cast and vector-hash kernels that dictionary arrays and
IPC need internally. `ARROW_COMPUTE=OFF` excludes the full kernel suite, not that core, and
there is no option that excludes it. D1 is therefore enforced by gma **never calling** an
Arrow kernel to produce a value, not by the kernels being absent from the archive. Worth
knowing before someone reads "compute is off" as a structural guarantee: it is a
scope-of-linkage fact, and the structural guarantee for D1 remains the SQL lint (§5.4).

### 15.2 Databento: decode it ourselves — ENC-1482, 2026-10-14

**Q14 resolved: `gma_data` decodes DBN itself and does not link `databento-cpp`.** The
deciding number is dependency weight, and it is bigger than "one more library":

| what `databento-cpp` requires | why we do not need it |
|---|---|
| OpenSSL, cpp-httplib | the historical/live **HTTP** API — ENC-1483's ingest job owns the download |
| nlohmann_json | the JSON half of that API |
| HowardHinnant/date | metadata date formatting |
| zstd | `.dbn.zst`, which the ingest job decompresses before gma sees it |
| **a Rust toolchain** | `databento-cpp` delegates DBN decoding to the Rust `dbn` crate's C-FFI (`src/detail/dbn_fsm.cpp` is a wrapper around `DbnDecoder_*`) |

That last row is the one that settles it: a Rust toolchain inside a C++20/CMake project that
vendors its dependencies under `third_party/` is a much larger step than Arrow was (§15.1 —
17 MB, 45 s), and D14 has just dropped Parquet and DuckDB *specifically* to keep the client
profile small. Meanwhile 90% of the SDK — `historical.cpp` (56 KB), `publishers.cpp` (61 KB),
`live_blocking.cpp` (22 KB) — is API surface the engine process would never call.

What we actually need is small and static: two packed PODs (56 and 48 bytes,
**byte-identical across DBN v1/v2/v3** — the vendor aliases `v1::MboMsg = v2::MboMsg =
v3::MboMsg`), a 100-byte fixed metadata block, and one variable-length symbology section.
`data/src/DatabentoAdapter.cpp` is ~700 lines including the comments.

**The honest cost.** DBN is a wire format we do not own and now have to track: a v4 lands on
us rather than on a vendor bump, and metadata/symbology is precisely where a hand-rolled
decoder goes wrong. Three mitigations, in the build rather than in a comment:

1. **The framing is measured, not remembered.** Five files of real Databento DBN bytes are
   committed at `data/tests/fixtures/dbn/` (Apache-2.0, from `databento-cpp`'s own test data —
   provenance and licensing in `PROVENANCE.md` there), and the test's first assertion is that
   a DBN **encoder** written in the test file reproduces each of them **byte for byte** from
   the values we believe they hold. One assertion, three facts: the layout is right, the
   values are right, the encoder is right — and only then is that encoder used to build the
   all-actions streams the two-record samples cannot reach. Expectations are cross-checked
   against `databento-cpp`'s `DbnDecoderSchemaTests`, so they have a second independent
   source.
2. **An unknown DBN version is refused, never guessed** — as are an undeclared `rtype`,
   `action` or `side`, a wrong record length, a zstd frame, `ts_out`, a non-zero metadata
   tail, and a partial record at end-of-stream (which is a *gap*, not an EOF).
3. **Acceptance is tested per enumerator, not just rejection.** Every declared `Action`,
   `Side` and `RType` is asserted to be accepted, enumerated from the arrays rather than
   spot-checked — a suite that only tests rejection cannot catch a bound that over-rejects a
   now-legal value, which is the bug ENC-1481 shipped and fixed.

A 16-mutation adversarial sweep over the decoder (swapped field offsets, a Cancel that stops
subtracting, a Fill side flip removed, `bookNeutral` cleared, a 99-byte metadata block, a
synthesized `provider_seq`, a closed symbology window, an unbounded order map) found **one**
guard with no test behind it — the metadata-tail check — which is now covered; every other
mutation was caught.

**What the DBN files taught us that this document had wrong or missing** is recorded in
SPEC §5 under Q14: the exact field names and offsets, the per-version metadata layout, and
**DBN v3's 8-byte metadata alignment padding** (7 zero bytes in the committed v3 fixture, 0
in the v2 one) — a detail nothing in this design anticipated and which would otherwise have
made every record read at a 7-byte offset.

**No system Arrow is available on this box**, which is why the source path is the measured
one. `pkg-config --modversion arrow` fails and no `/usr/lib64/cmake/arrow` exists. Fedora
44 *does* package it (`libarrow-devel` 23.0.1 — so the common claim that `dnf` only has the
unrelated `arrows` package is wrong), but installing it needs a sudo password this
environment does not have. `find_package(Arrow CONFIG QUIET)` is tried first and kept
working for machines that have one; it is **untested here** for that reason.
`-DGMA_DATA_ARROW_FORCE_FETCH=ON` skips it — the escape hatch for an installed Arrow that
is present but unusable (a config package exporting target names outside the candidate
list makes the module a `FATAL_ERROR`, and that error used to name this flag before it
existed).

---

## 16. Open questions

**Resolved in this revision:** Q2 (self-hosted single node, local machine) · Q3 (→ C4, one
emission rule from the planner) · Q4 (→ C9, global 1e-9) · Q6 (→ C12 dissolved; now a
granule-skipping measurement, T6) · Q9 (→ §8.5, snapshots required) · Q12 (→ §8.7, DBN
retained; L1 is a rebuildable projection) · Q13 (→ §14.2, `Interval` is live-only) ·
Q16 (→ this doc is the deep reference; `specs/2026-10-14-gma-data-layer/SPEC.md` is the
locked-decision record).

**Still open:**

**Q1 — `bucketStartMs` under event time.** Amend `specs/2026-09-20-timestamps-on-the-wire`
D9/D10, or carry both bases on the wire? I would amend: two time bases on one frame is how
charts disagree with themselves. *Blocks the wire half of event time, not the first slice.*

**Q5 — determinism scope.** Bit-identical for *all* graphs, or only those avoiding the
documented execution-time-sampling case (C11)? Honest answer today is the latter; the
fixture should say so out loud.

**Q7 — cross-symbol graphs and sharding.** Unsharded, or a symbol-group shard, and who
picks the group? *With §7.*

**Q8 — long-interval fill.** Accept the scan, or make the one narrow exception to §8.1?
*After B3.*

**Q10 — chart fill depth at scene-init.** Accept bounded attach-with-N-bars, or change the
wire to support rebasing? I lean accept — rebasing means solving ENC-1101's buffer-zeroing.

**Q11 — persist derived series as a cache?** Optional now. If yes, invalidation key is graph
identity.

**Q14 — ~~Databento client library, or decode DBN directly?~~ RESOLVED by T4 (ENC-1482):
decode it ourselves.** The reasoning, the cost stated in both directions, and the mitigations
are **§15.2**; the facts the real DBN bytes settled are SPEC §5 under Q14.

**Q15 — the C13 double-count refusal.** Confirm the planner should **refuse** a graph
reading `trades` and `book_events` for the same symbol and window, rather than attempt
reconciliation. I recommend refuse: reconciliation is a semantic choice masquerading as a
convenience.

---

## 17. First slice

Nine tickets to a measured answer. Full detail and ticket ids:
**`specs/2026-10-14-gma-data-layer/SPEC.md` §6.**

| # | ticket | repo | deps |
|---|---|---|---|
| T1 | `stack.sh up clickhouse` — rootless podman, single node, memory-capped | workspace | — |
| T2 | L1 DDL — trades, book_events, book_snapshots, symbology, ingest_windows, gaps | GMA_V3 | T1 |
| T3 | `gma_data` skeleton + canonical `Schema.hpp` + CMake target | GMA_V3 | — |
| T4 | Databento DBN adapter — trades + mbo → canonical batches | GMA_V3 | T3 |
| T5 | ingest job — 7d/1symbol pull → L1, retain DBN, completeness rows | GMA_V3 | T2, T4 |
| T6 | measure `book_events` sort-key uniqueness against real MBO | GMA_V3 | T5 |
| T7 | **B1** — ClickHouse → gma transport benchmark, native vs HTTP | GMA_V3 | T5 |
| T8 | `Dispatcher::onRecord` — non-JSON ingress | GMA_V3 | T3 |
| T9 | **B2** — rows/sec through the DAG: JSON vs `onRecord` vs inline vs interned | GMA_V3 | T7, T8 |

The 21-ticket plan from revision 2 is the **post-measurement** plan, not the starting one.
What T9 reports decides which of §6's changes are worth building and whether §7's sharding
is needed at all.
