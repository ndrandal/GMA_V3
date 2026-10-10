# GMA_V3 Data Layer — one DAG over live, warm-up and replay

**Status:** design. Nothing here is implemented. ENC-1478.
**Audience:** whoever implements the follow-on tickets; read §2 before writing code.

---

## 0. TL;DR

A new library target, `gma_data`, sits **upstream** of the engine and owns everything
gma does not: provider protocols, normalization, durability, and history. It emits
canonical Arrow record batches carrying `(event_time_ns, provider_seq, symbol)` and
hands the engine *decoded columns*, never a JSON DOM. A `SplicedSource` performs the
backfill → replay → drain → pass-through handoff once, in the source layer, so no graph
ever knows whether its data is live. History lives behind `HistoryBackend`, defaulting
to Parquet-on-disk read through embedded DuckDB, with a `clickhouse-server` backend
available for shared TB-scale deployments and no gma code change between the two.

Five things in the current engine have to change for this to work at all, and §2 names
them. The largest is that **event time does not exist in GMA_V3 today** — the three
timer nodes are wall-clock threads and `StreamValue` carries no event timestamp. Until
that is fixed, "the same DAG runs identically on live and replay" is false for every
graph that contains a window.

---

## 1. What exists today

Mapped from `master` @ `90f0778`. Read `docs/ARCHITECTURE.md` for the full picture; this
is the subset the data layer touches.

### 1.1 Ingest

```
TCP :9001 ──▶ FeedServer::handleLine ──┐
                                        ├──▶ Event{symbol, shared_ptr<rapidjson::Document>, type}
wss:// ──▶ WsFeedClient ──▶ IFeedAdapter│         │
            ::translate() ──▶ FeedEvent ┘         ▼
                   │                      Dispatcher::onTick
                   └──▶ OrderBookManager (book mutations, seq/epoch)
```

- **`engine::IIngressSource`** (`include/gma/engine/IngressRegistry.hpp`) — `start()` /
  `stop() noexcept`. Named factories register on `IngressRegistry`
  (`market.feedserver`, `market.wsclient`); the composition root instantiates one per
  `cfg.ingress[]` entry (ENC-31). **This is the right seam and the design reuses it.**
- **`gma::feed::FeedEvent`** (`connectors/market/include/gma/feed/FeedEvent.hpp`) — a
  `std::variant` over `TickEvent`, `ObAdd/Update/Delete/Trade/TickSize/ResetEvent`.
  Already carries `timestampNs` on `TickEvent`/`ObTradeEvent` and
  `std::optional<uint64_t> seq` on the five OB variants. This is much closer to the
  canonical model than I expected, and it is the natural ancestor of the Arrow schema in
  §5.
- **`gma::Event`** (`include/gma/Event.hpp`) — `{symbol, shared_ptr<rapidjson::Document>
  payload, type}`. The *engine's* ingress unit, and it is a JSON DOM.
  `Dispatcher::onTick` iterates `payload->MemberBegin()..MemberEnd()` per tick
  (`src/core/Dispatcher.cpp:138`); `MarketTickComputer::compute` does
  `doc.HasMember(fieldMap.priceFields[i])` lookups per tick
  (`connectors/market/src/MarketTA.cpp:342-367`).
- **`OrderBookManager`** (`connectors/market/include/gma/book/OrderBookManager.hpp`) —
  already has the L2/L3 machinery this design needs: `onSeq(symbol, seq)` returning
  "may I apply this", per-symbol `FeedState{lastSeq, epoch, stale}`, `onReset(symbol,
  epoch)`, a `requestSnapshotFn` callback invoked on gap/reset, and
  `onSnapshotPerOrder` / `onSnapshotAggregated` that accept an optional `snapshotSeq` to
  resume from. Prices are `Price{int64_t ticks}` internally, quantized per symbol
  (`kDefaultTick = 1e-4`), but every public entry point is `double`.

### 1.2 DAG execution

- **`Dispatcher`** routes `Event` by `type` to `IEventComputer`s, then fans raw payload
  fields to `(symbol, field)` listeners. Writes derived scalars into `AtomicStore`.
- **`AtomicStore`** — latest-value-wins `(symbol, field) -> ArgType`, `shared_mutex`.
- **`StreamValue{symbol, ArgType value, int64_t bucketStartMs}`** is the value on every
  pipeline edge. `bucketStartMs` is *wall-clock bucket identity*, stamped only by
  `TumblingWindow`/`BucketTime`, `0` meaning "not bucketed".
- **Clocking.** Three nodes drive time, all off the host clock:
  `BucketTime` (`system_clock::now()` + `wait_until` on a period boundary,
  `src/nodes/BucketTime.cpp:70`), `TumblingWindow` (same, `src/nodes/TumblingWindow.cpp:76`),
  `Interval` (`cv.wait_for(period)`, `src/nodes/Interval.cpp:33`). Each owns a
  `std::thread` that fires whether or not data arrived.
- **Ordering.** ENC-1005 gives each request DAG an `rt::Strand`; `Dispatcher::deliver`
  calls a strand-bearing `Listener` inline and posts everything else to the
  `ThreadPool`. Per-DAG order holds **only under single-threaded ingress**, which
  `src/main.cpp` provides by running one `ioc.run()` thread — stated as a precondition in
  `Dispatcher.hpp`, not enforced.
- **Pull sampling.** ENC-1335: `INode::samplesAtClock()` / `bindAtClock()` — an
  `AtomicAccessor` adjacent to a clock hop takes its store sample on the ingress thread,
  because the store is only coherent with the tick there.
- **Egress.** `Responder` → `ClientSession` outbox, which is **deliberately lossy**:
  `update` frames coalesce above 256 queued and displace the oldest pending `update` at
  4096 (ENC-996 / ENC-1072).

### 1.3 What does not exist

No Arrow, Parquet, DuckDB, chDB or ClickHouse anywhere in the tree (`third_party/` holds
only `rapidjson`). No history, no replay, no backfill, no persistence of any kind — the
only `replay` in the codebase is config-key replay (`Config::dispatchPendingKeys`). Event
timestamps are extracted (`MarketTA.cpp:364`) and then **stored as a string atomic**
(`ctx.store->set(symbol, "timestamp", std::to_string(tsNs))`, `MarketTA.cpp:392`) and
read by nothing.

---

## 2. Where this design conflicts with what exists

Ranked by how much work the conflict is, not by how bad it looks. C1–C5 are blocking;
C6–C11 are real but local.

### C1 — Event time does not exist, and the clock nodes are wall-clock threads

> *"Operators must key off event time only, never wall clock."*

`StreamValue` has no event-time field. The only time-ish member, `bucketStartMs`, is
documented as wall-clock-derived and is produced by `BucketTime::nextAlignedAfter(
system_clock::now(), period)`. All three timer nodes own a thread that fires on the host
clock independent of data arrival.

Consequence: replay a 10-minute window through a graph containing `TumblingWindow(60s)`
and you get **one** bar (the replay finishes in under a second of wall time), not ten.
The outputs are not merely different, they are a different *number* of outputs, so no
amount of tolerance in a diff recovers parity.

What it takes:
1. `StreamValue` gains `int64_t eventTimeNs`. It is an aggregate with a default member
   initializer, so adding a third defaulted member keeps every `StreamValue{sym, val}`
   braced-init site compiling — the same property ENC-1280 relied on.
2. Every value-transforming node copies it through, exactly as they already copy
   `bucketStartMs`.
3. `TumblingWindow` / `BucketTime` get an **event-time driver**: boundaries are crossed
   when an arriving value's `eventTimeNs` (or a watermark) passes them, not when a thread
   wakes. The wall-clock thread survives in live mode only, as the "close a quiet bar"
   path, and must be *disabled* in replay.
4. `bucketStartMs` is re-derived from event time. **This is a wire-visible semantic
   change** against `specs/2026-09-20-timestamps-on-the-wire/SPEC.md` D9/D10, which
   explicitly fixes it to the wall-clock boundary the timer aligned to. That SPEC needs
   amending, not working around. → **Q1**.

### C2 — The engine's ingress unit is a JSON DOM, so "no row-by-row conversion" cannot hold through `Dispatcher`

> *"Arrow end to end with no row-by-row conversion at boundaries."*

`gma::Event` carries `shared_ptr<rapidjson::Document>`. `Dispatcher::onTick` and
`MarketTickComputer::compute` both walk it by member name, per tick. A data layer that
produced Arrow and then serialized to a DOM to feed `onTick` would be strictly worse than
what exists.

Arrow *inside* the DAG is a different proposition again: `ArgType` is a scalar variant,
`INode::onValue` takes one `StreamValue`, and there are ~25 node types. Columnar
execution is a rewrite of the whole pipeline and is not what this ticket buys.

**The design draws the boundary explicitly.** Arrow is the currency of the data layer —
adapter → normalizer → sink → history → replay — and there is **exactly one**
columnar→scalar decode point, in `ISource`, which walks a batch column-wise into a POD
`TickRecord` and calls a new, non-JSON `Dispatcher` entry point. No DOM is constructed on
the Arrow path at all, which is a net *reduction* in per-tick work versus today. Claiming
more than that would be false. → **Q2** on whether that boundary is acceptable or whether
columnar node execution is in scope eventually.

Concretely, `Dispatcher` needs a second ingress overload:

```cpp
void onTick(const Event& tick);                 // existing JSON path, unchanged
void onRecord(const data::TickRecord& rec);     // new: resolved fields, no DOM
```

`Event` and the JSON path stay for the legacy TCP/`market.wsclient` ingress. Two ingress
shapes is a cost; folding them is a bigger one.

### C3 — No sequence number on the engine value path, and none on `TickEvent`

Splice step 4 is *"drain the buffer, dropping seq ≤ last replayed."* Today
`feed::TickEvent` has **no** `seq` field (only `timestampNs`); the five OB variants have
`std::optional<uint64_t> seq`. `gma::Event` has no seq. So the dedup predicate the splice
is built on cannot be evaluated for trades or quotes.

Fix: `provider_seq` is **mandatory and non-optional** in the canonical schema (§5), and
the normalizer is responsible for synthesizing one when a provider does not supply it
(§4.4). Carrying it `optional` through the splice means every dedup site has a
"what if absent" branch, and those branches are where the duplicate-bar bugs live.

### C4 — Warm-up and replay are different modes, and the engine has neither

`MarketTickComputer::_symbolHistories` (`deque<TickEntry>`, cap `taHistoryMax`=1000) and
`Dispatcher::_histories` (per-`(symbol,field)` `deque<double>`) are populated **only** by
`onTick`. There is no way to advance that state without also firing every listener.

So "fetch history, replay it through the DAG" has two distinct meanings and they want
opposite behaviour:

| mode | state | output |
|---|---|---|
| **warm-up** — prime a 200-period SMA before going live | advance | **suppress** |
| **replay / backtest** — run the graph over a past window | advance | **emit** |

Without the suppress mode, a client subscribing to a 200-bar SMA receives 200 spurious
`update` frames on connect, each a partially-warm indicator. `ISource` therefore carries
an explicit mode and the engine needs an emission gate keyed on it. → **Q3**.

### C5 — Replay parity cannot be measured at the WebSocket boundary

`ClientSession`'s outbox coalesces `update` frames above 256 queued and displaces the
oldest pending `update` at 4096. `docs/ARCHITECTURE.md` §8 says so in as many words: a
consumer doing *event replay* "cannot rely on the `update` stream when it falls behind."
A replay that outruns the test harness therefore drops frames **by design**, and the
parity diff reports a mismatch that is not a defect.

Parity is captured **upstream of the outbox**, at the `Responder`'s send function, by a
`RecordingSink` (§7) that writes every emitted value to an Arrow file with no
coalescing and no queue. The WS path is not on the parity critical path at all.

### C6 — Deterministic replay needs an explicit quiesce protocol, not a sleep

`Dispatcher::deliver` posts to `ThreadPool` for every node that does not hold a strand.
Per-DAG order holds under one ingress thread (ENC-1005) and `src/main.cpp` provides
exactly that, but `Dispatcher.hpp` records it as a precondition rather than enforcing it,
and notes it is "one `for (...) threads.emplace_back([&]{ioc.run();})` away" from being
broken.

Replay runs must therefore: assert single-threaded ingress, set `threadPoolSize=1`, and
end with `ThreadPool::drain()` — not a sleep. ENC-1340 already established that
`drain()` is the deterministic barrier and a longer sleep is "not a fix, it is a wider
window". `ReplaySource::run()` returns only after a final drain.

### C7 — `IIngressSource` has no completion signal and no clock

`start()` / `stop() noexcept` is right for an endless live feed and wrong for a replay,
which *finishes*. A replay source also has to own the event-time clock while it runs. Two
additions, both on a new interface in `gma_data` rather than on
`engine::IIngressSource`, which `SplicedSource` then adapts to.

### C8 — Fixed-point scale: DBN is global 1e-9, gma's is per-symbol runtime

Databento DBN prices are `int64` at a fixed 1e-9 scale. `OrderBookManager` quantizes to
`Price{int64_t ticks}` using a **per-symbol, runtime-configured** tick
(`setTickSize`, default `1e-4`), and every public entry point takes `double`.

Recommendation: the canonical schema uses a **single global scale, 1e-9**, so there is no
per-row scale column and no per-symbol lookup on decode. Per-symbol tick size stays what
it is — a book-quantization concern inside `OrderBookManager`. That leaves one
`int64 → double` conversion at the `OrderBookManager` entry points, which should
eventually gain `Price`-typed overloads to remove it. Deferred, not blocking. → **Q4**.

### C9 — Arrow must not become a `gma_engine` dependency

`gma_engine`'s PUBLIC include path is `include/` only, enforced by CMake include scoping,
and the repo's stated invariant is that adding a data source touches no engine code. The
footprint constraint ("embedded or single-binary, nothing ops-heavy") makes this sharper:
Arrow + Parquet + DuckDB are each large enough that linking them into the engine would
dominate the binary.

The design keeps all of it in `gma_data`. `gma_engine` gains the event-time field and
the `onRecord` entry point — plain POD, no new dependency. §11 is the dependency budget
and it has unmeasured numbers in it that need measuring before anything is vendored.

### C10 — `AtomicStore` is a mutable latest-value store, and pull nodes race it

ENC-1335 exists because the store is written by the ingress thread one statement before
the clock is posted, so a pull node sampling later reads a *different* tick's value. The
`bindAtClock` fix binds the sample to the clock on the ingress thread. Replay does not
break this — it is the same mechanism with a different thread driving it — but it does
mean **replay determinism inherits the ENC-1335 contract**: a pull node not adjacent to a
clock hop keeps execution-time sampling and so is *not* replay-deterministic. That is
existing documented behaviour, and the parity test must either avoid such graphs or
record the non-determinism as expected. → **Q5**.

### C11 — The project framing contradicts the workspace's locked model

This is a documentation conflict, not a code one, and it will cause the next reader to
design the wrong thing. The brief says *"deploys inside client environments; data
sovereignty is the core differentiator"* and names Databento Nasdaq + OTC equities.
The workspace's locked model
(`specs/2026-06-24-auth-identity-cognito/SPEC.md`, summarised in the workspace
`CLAUDE.md`) says the platform pivoted in 2026-06 to **crypto-first, centrally hosted,
zero-PII**, with the sovereignty framing explicitly superseded and `embassy`'s on-prem
role under review.

Both cannot be the plan. The *data layer* design is largely indifferent — swappable
provider adapters and a no-required-services default serve either — but two decisions
turn on it: whether the ClickHouse backend is a real deployment target or dead weight,
and whether the canonical schema needs equity-specific fields (OTC venue codes, lot
conventions, halts) at all. → **Q11**, and it is the one I would answer first.

---

## 3. Components and data flow

```
┌─ gma_data ───────────────────────────────────────────────────────────────────┐
│                                                                             │
│  IProviderAdapter          INormalizer                                      │
│  ┌──────────────┐          ┌──────────────────────────────────────┐         │
│  │ Databento    │  raw     │ decode → canonical columns →          │         │
│  │ DBN / JSON   │ ───────▶ │ arrow::RecordBatch                    │         │
│  ├──────────────┤  frames  │  (event_time_ns, provider_seq, symbol)│         │
│  │ IQFeed       │          └───────┬───────────────────┬──────────┘         │
│  ├──────────────┤                  │                   │                     │
│  │ feed-sim     │                  │ live tail         │ batched            │
│  └──────────────┘                  ▼                   ▼                     │
│                          Arrow IPC stream        IHistorySink               │
│                          (bounded ring file)           │                     │
│                                  │                     ▼                     │
│                                  │            ┌──────────────────┐          │
│                                  │            │ IHistoryBackend  │          │
│                                  │            ├──────────────────┤          │
│                                  │            │ Parquet+DuckDB ◀─┤ default  │
│                                  │            │ ClickHouse     ◀─┤ optional │
│                                  │            └────────┬─────────┘          │
│                                  │                     │ query(range)        │
│                                  ▼                     ▼                     │
│                      ┌───────────────────────────────────────────┐          │
│                      │ ISource                                   │          │
│                      │   LiveSource    — tail only               │          │
│                      │   ReplaySource  — history only, finishes  │          │
│                      │   SplicedSource — history ▸ buffer ▸ live │          │
│                      └──────────────────┬────────────────────────┘          │
└─────────────────────────────────────────┼───────────────────────────────────┘
                                          │ TickRecord / BookRecord (POD, column-decoded)
                                          ▼
┌─ gma_engine ────────────────────────────────────────────────────────────────┐
│  Dispatcher::onRecord ──▶ IEventComputer ──▶ AtomicStore                     │
│                       └─▶ Listener ──▶ … event-time clocked nodes … ──▶      │
│                                                       Responder ──▶ WS       │
│                                                       SinkNode  ──┐          │
└───────────────────────────────────────────────────────────────────┼─────────┘
                                                                    │ batched
                                                                    ▼
                                                        IHistorySink (derived)
```

Five rules that keep this honest:

1. **gma never owns durability.** The normalizer writes; gma reads. A gma crash loses no
   raw data because gma was never the thing holding it.
2. **History reaches gma only at graph boundaries** — source construction (warm-up) and
   replay. Never per tick, never from inside a node.
3. **One decode point.** Arrow → POD happens in `ISource`, column-wise, once per batch.
4. **The splice lives in `ISource` and nowhere else.** No graph, node or computer
   contains a branch on "am I live".
5. **`gma_data` depends on `gma_engine`, never the reverse.** Same direction as a
   connector.

Where `gma_data` sits relative to the existing connector split: it is a peer of
`connectors/market/`, not part of it, because the market connector owns *book and TA
semantics* while `gma_data` owns *transport, encoding and durability*. The market
connector will depend on `gma_data` (it needs `TickRecord`/`BookRecord` to feed
`OrderBookManager`). `gma_engine` depends on neither.

---

## 4. The live/historical splice

The hard part. One implementation, in `SplicedSource`, driving a five-state machine.

### 4.1 States

```
  BUFFERING ──▶ BACKFILLING ──▶ REPLAYING ──▶ DRAINING ──▶ LIVE
      │                                                     ▲
      └─────────────────── (gap / disconnect) ──────────────┘
                                   │
                                   └──▶ RESYNC ──▶ BACKFILLING
```

| state | what happens |
|---|---|
| `BUFFERING` | Live subscription is **open** and every batch is appended to a bounded in-memory buffer. Nothing reaches the engine. Records the first live `(provider_seq, event_time_ns)` per symbol — the **splice point**. |
| `BACKFILLING` | `HistoryBackend::query(symbols, T-lookback, splicePoint)` streams batches. Half-open interval: `[T-lookback, splicePoint)` by seq where seq is trustworthy, by ts where it is not (§4.4). |
| `REPLAYING` | History batches are decoded into the engine in event-time order. Emission is gated by the source's mode (§4.5). |
| `DRAINING` | Buffered live batches are decoded, **dropping every record with `provider_seq <= lastReplayedSeq[symbol]`**. The buffer may still be growing; drain until it is empty *and* the live callback has been switched. |
| `LIVE` | Pass-through: batches go straight from adapter to engine. Buffer is released. |

The `BUFFERING`→`BACKFILLING` ordering is not an implementation detail — subscribing
*before* querying is the whole reason the splice has no hole. Query first and the window
between "history ends" and "live begins" is lost silently.

### 4.2 The handoff must be atomic per symbol, not per source

Symbols splice independently: `AAPL` may reach `LIVE` while `MSFT` is still
`BACKFILLING`, because the history query returns at different depths and the first live
record arrives at different times. `lastReplayedSeq` and the state are therefore
**per-symbol**, and the source-level state is the minimum over symbols. A single
source-wide state machine is the obvious wrong simplification — it stalls every symbol
behind the slowest one, which on an OTC feed is a symbol that may not print for an hour.

### 4.3 Buffer bound and the overflow decision

The buffer is bounded (records or bytes, per symbol). Overflow during a slow backfill is
the interesting failure: the options are drop-oldest (a hole, silently wrong), drop-newest
(a hole at the splice, silently wrong), or **abort the splice and restart it** with a
later `T`. Only the third is safe. `SpliceConfig::maxBufferedRecordsPerSymbol` plus an
abort-and-retry with bounded attempts, and a counter per abort — a deployment that aborts
repeatedly has a backfill that is too slow for its feed rate and the operator needs to
see that, not a quietly thin chart.

### 4.4 Sequence numbers, and what to do when the provider has none

Dedup by `provider_seq` is exact and dedup by timestamp is not (ties are real: two trades
at the same nanosecond on the same symbol are ordinary). Three cases:

- **Provider gives a monotonic per-symbol seq** (DBN does, within a session). Use it.
- **Provider gives a monotonic global seq** (per channel, not per symbol). Use it; it is
  still a total order and still exact for dedup.
- **Provider gives none.** The normalizer **synthesizes** one: a per-`(symbol, session)`
  counter assigned at normalization time, written into the canonical column, and
  persisted. It is then exact for everything downstream *of that normalizer*, which is
  what dedup needs. It is **not** comparable across normalizer restarts, so the schema
  carries `session_id` (§5) and the dedup key is `(session_id, provider_seq)`.

The `session_id` is what makes a mid-session restart tractable (§8.4). Synthesizing a seq
rather than making it optional is C3's resolution: one branch, in one place, at
normalization time.

### 4.5 Warm-up vs replay emission

`SpliceConfig::replayMode`:

- `Suppress` — advance computer and `Dispatcher` history state, do not deliver to
  listeners. For live warm-up. Needs the engine emission gate (C4).
- `Emit` — deliver everything. For replay/backtest and for the parity test.

Default `Suppress` for `SplicedSource`, `Emit` for `ReplaySource`. → **Q3**.

### 4.6 L2/L3: snapshot, deltas, gap, re-snapshot

`OrderBookManager` already implements most of this and the data layer should drive it
rather than duplicate it:

1. Replay from history begins with a **snapshot** at or before the start of the window —
   `onSnapshotPerOrder(symbol, orders, snapshotSeq)` or `onSnapshotAggregated(...)`,
   both of which already take the resuming seq.
2. Deltas apply through `onAdd`/`onUpdate`/`onDelete`, each gated by
   `onSeq(symbol, seq)` which returns false on a gap.
3. On gap, `OrderBookManager` marks the symbol stale and invokes `requestSnapshotFn`.
   **The data layer supplies that callback**, and it does one of two things: fetch the
   nearest snapshot from history (replay), or request a fresh snapshot from the provider
   (live). Same callback, two implementations, chosen by the source.
4. `onReset(symbol, epoch)` bumps the epoch, which `OrderKey` already carries, so
   pre-reset orders cannot be matched by post-reset messages.

What is missing: **snapshot persistence**. A replay of an arbitrary window needs a book
snapshot at its start, and reconstructing one by replaying from session open is O(session)
per backtest. So the sink writes **periodic book snapshots** alongside deltas (§5.4), and
`HistoryBackend::latestSnapshotAtOrBefore(symbol, ts)` is part of the interface. Snapshot
cadence is a storage/replay-cost tradeoff. → **Q6**.

---

## 5. Canonical schema

Full Arrow field definitions: `data/include/gma/data/Schema.hpp`.
Matching DDL: `data/schema/clickhouse.sql`.

Three tables, deliberately not one. A unified event table forces nullable columns for
every field any event type lacks, which defeats the codecs and makes every read scan
columns it does not want.

### 5.1 Common columns (every table)

| column | Arrow type | notes |
|---|---|---|
| `ts_event` | `timestamp(NANO, "UTC")` | **event time** — the venue's timestamp. The only time any operator may key off. |
| `ts_recv` | `timestamp(NANO, "UTC")` | normalizer receipt time. Diagnostics and latency only; **never** an operator input. |
| `provider_seq` | `uint64` non-null | monotonic within `(session_id, symbol)`. Synthesized if the provider has none (§4.4). |
| `session_id` | `uint32` | normalizer session. Makes `provider_seq` comparable across restarts (§8.4). |
| `symbol` | `dictionary<int32, utf8>` | dictionary-encoded: the cardinality is thousands against millions of rows. |
| `venue` | `dictionary<int16, utf8>` | Nasdaq / OTC / … |

Prices are **`int64` at a fixed global scale of 1e-9**, matching DBN, with no per-row
scale column (C8). `GMA_PRICE_SCALE` is a compile-time constant.

### 5.2 `trades`

`price int64`, `size uint64`, `aggressor uint8` (maps to the existing
`gma::Aggressor`), `book_neutral bool` (maps to `ObTradeEvent::bookNeutral` — hidden /
non-displayable prints that must not consume resting liquidity), `trade_id uint64`,
`flags uint16`.

### 5.3 `quotes` (L1)

`bid_px int64`, `ask_px int64`, `bid_sz uint64`, `ask_sz uint64`,
`bid_ct uint32`, `ask_ct uint32`. One row per top-of-book change, not per tick.

### 5.4 `book_events` (L2/L3) and `book_snapshots`

`book_events`: `action uint8` (add/update/delete/trade/clear/ticksize — the existing
`FeedEvent` variant set), `side uint8` (`gma::Side`), `price int64`, `size uint64`,
`order_id uint64`, `priority uint64`, `depth uint16`, `epoch uint32`, `flags uint16`.

`book_snapshots`: `epoch uint32`, `levels list<struct{side uint8, price int64, size
uint64, order_count uint32}>`. Written on cadence (§4.6, **Q6**) so a replay can seek.

### 5.5 ClickHouse DDL shape

Per `data/schema/clickhouse.sql`:

```sql
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
```

with codecs as specified: `DoubleDelta` on `ts_event` / `ts_recv` / `provider_seq`,
`Delta(8), ZSTD(3)` on every price column, `T64, ZSTD(1)` on sizes.
`DateTime64(9, 'UTC')` for timestamps.

One caveat on `ORDER BY (symbol, ts_event, provider_seq)`: it is right for the read
pattern (always "this symbol, this time range") and it means inserts are **not** in
primary-key order when a batch spans symbols, so `MergeTree` sorts each part on insert.
That is the correct trade for a read-heavy store, but batches should be symbol-sorted
before insert to keep part merges cheap.

The Parquet layout mirrors it: `partition by (symbol, date)`, rows sorted by
`(ts_event, provider_seq)` within a file, so DuckDB prunes on both the path and the row
group.

---

## 6. History backends

`IHistoryBackend` (`data/include/gma/data/IHistoryBackend.hpp`) exposes four operations
and nothing else:

```
query(symbols, [from, to), kinds)      -> batch stream
latestSnapshotAtOrBefore(symbol, ts)   -> one batch
append(batch)                          -> void          (sink side)
capabilities()                         -> struct
```

**Default: `ParquetHistoryBackend`.** Parquet files on local disk, queried through
embedded DuckDB. No server, no daemon, no port — satisfies "no new required services in
the default deployment". DuckDB reads Parquet natively and returns Arrow, so there is no
row-by-row conversion on the read path either.

**Optional: `ClickHouseHistoryBackend`.** For shared, concurrent, TB-scale stores. Two
transport choices and the design does not need to pick now: native protocol via
`clickhouse-cpp`, or HTTP with `FORMAT ArrowStream`. HTTP costs a little throughput and
saves a dependency; native is the opposite. → **Q7**.

`capabilities()` exists because the two backends genuinely differ — concurrent writers,
server-side predicate pushdown, retention/TTL — and the alternative to reporting the
difference is pretending it away and having the splice behave differently per deployment.
**gma code does not branch on it**; only operator-facing startup validation does.

chDB is noted and not chosen: it is DuckDB's niche with ClickHouse's SQL dialect, and
carrying it as a third option buys dialect compatibility with the optional backend at the
cost of a second embedded engine. → **Q8**.

---

## 7. Sinks (gma → history), optional

A `SinkNode` is an ordinary `INode` that terminates a chain, accumulates emitted
`StreamValue`s into an Arrow builder, and flushes a `RecordBatch` to an `IHistorySink` on
a size or event-time-interval trigger. Used for derived outputs — bars, signals,
features — and **never** on the raw path: raw durability is the normalizer's job (§3
rule 1).

It is optional in the strict sense: no sink in a graph means no history dependency at
runtime, and a build without `gma_data` still produces a working `gma_server`.

`RecordingSink` is the same mechanism with a fixed schema `(request_key, symbol,
event_time_ns, bucket_start_ms, value)`, used by the parity test (§10) and by session
recording. It is placed at the `Responder`'s send function, upstream of `ClientSession`'s
lossy outbox (C5).

---

## 8. Failure modes

### 8.1 Provider disconnect

`WsFeedClient` already reconnects with exponential backoff to 30s. On reconnect the feed
resumes at an unknown point, so the gap is real even though the socket recovered:

- Source re-enters `BUFFERING` → `BACKFILLING` from `lastDeliveredEventTime[symbol]` to
  the new splice point, i.e. **the ordinary splice, run again**. This is the main reason
  the splice is a reusable state machine rather than a startup script.
- If history has not yet been written for the gap window (the normalizer's sink flushes
  on a batch boundary), the backfill returns short. The source must then either wait for
  the sink to flush or mark the window `degraded`. Waiting is correct; the sink exposes
  `flushedThrough(symbol) -> ts` so the source can tell the difference between "no data"
  and "not written yet". **This distinction is the one that silently produces wrong
  charts if omitted.**
- Live-only deployments with no history configured cannot splice. They drop to a
  `gap` marker on the stream and the operator sees a counter. Do not interpolate.

### 8.2 Sequence gaps

Trades/quotes: a gap in `provider_seq` is detected by the normalizer and recorded as a
`gap` row (or a metric plus a log line — **Q9**). Do not stall the stream; a gap in a
trade feed is a missing print, not corruption.

Books: `OrderBookManager::onSeq` already returns false and marks stale, and
`requestSnapshotFn` fires. The data layer's callback re-snapshots (§4.6 step 3). A stale
book must not serve `ob.*` reads as if fresh — `isStale(symbol)` exists; the provider
namespace should consult it. *(Whether `ob::Provider` currently does is worth checking
during implementation; I did not verify it.)*

### 8.3 Out-of-order records

Two separate problems:

- **Within a symbol, from one provider.** Should not happen; if it does, the normalizer
  has a bug or the provider does. Count it, log it, and pass it through in arrival order
  — reordering on a bounded window hides the defect and adds latency.
- **Across sources during a splice.** Handled by the seq-based drop in `DRAINING`, which
  is exact (§4.4), not by a reorder buffer.

Operators keying off event time (C1) are tolerant of *late* data only to the extent their
watermark allows. Watermark policy — how long a window waits past its boundary before
closing — is the one piece of event-time semantics this doc does not settle. → **Q10**.

### 8.4 Restart mid-session

The normalizer restarts: `session_id` increments, `provider_seq` restarts from 0 (if
synthesized). Dedup keyed on `(session_id, provider_seq)` therefore stays exact across the
restart, and the splice's `lastReplayedSeq` comparison must carry the session — comparing
bare seqs across a restart is the bug this column exists to prevent.

gma restarts: it owns no durable state, so it re-splices from scratch. Warm-up cost is a
history query, which is the design working as intended.

The history store restarts: `ParquetHistoryBackend` is files, so it recovers to its last
flushed batch; a partially written Parquet file is discarded on open (footer missing) and
the window it covered is lost. The Arrow IPC live tail is the mitigation — it is written
record-at-a-time and is readable after a crash — and the recovery path is "replay the
tail into the sink". → **Q12**.

---

## 9. In scope vs deferred

**In scope (this design, then its tickets):**
- `gma_data` library: provider adapter, normalizer, Arrow canonical schema.
- Arrow IPC live tail + batched history sink.
- `IHistoryBackend` with `ParquetHistoryBackend` (DuckDB) complete, ClickHouse stubbed.
- `ISource`: `LiveSource`, `ReplaySource`, `SplicedSource`, registered as `IngressRegistry`
  kinds so wiring is an INI edit.
- Event time in `StreamValue` + event-time driving for `TumblingWindow` / `BucketTime` /
  `Interval` (C1 — this is the big one, and it is several tickets).
- `Dispatcher::onRecord`, the non-JSON ingress path (C2).
- Emission gate for warm-up suppression (C4).
- Databento adapter against the affordable subscription tier; feed-simulator adapter
  (it is the local default feed and the obvious parity fixture source).
- Replay-parity CI test (§10).
- `SinkNode` + `RecordingSink`.

**Deferred, named:**
- Columnar execution inside the DAG. One decode point only (C2).
- `ClickHouseHistoryBackend` implementation beyond the stub.
- chDB (Q8).
- Live TotalView / Databento Plus. The adapter interface is designed for it; the
  subscription is not bought.
- Multi-provider merge for one symbol (two feeds, one book). The schema carries `venue`
  so it is not foreclosed, but conflict resolution is not designed.
- Corporate actions, adjusted prices, symbol-change mapping. Real and out of scope; they
  belong to a reference-data layer, not this one.
- Compaction / retention policy for the Parquet store.
- Streaming tail from ClickHouse (it is a store, not a bus).
- `Price`-typed `OrderBookManager` overloads to remove the last `double` round trip (C8).

---

## 10. Replay-parity test plan

The claim under test: **for a fixed fixture window, a DAG's output when driven live is
byte-identical to its output when replayed from history.** Not "close" — identical. A
tolerance is an admission that the design does not hold.

### 10.1 Recording (one-time, by hand, committed)

1. `tools/stack.sh up feed` (feed-simulator — already the default local source, and it is
   deterministic, which a real provider is not).
2. `gma_server` with the data layer configured: normalizer writing both the Arrow IPC
   tail and the Parquet sink.
3. Subscribe a fixed set of DAGs from a committed corpus file. `tests/treebuilder/
   corpus_requests.json` (272 entries) is the obvious source — it already exercises
   fan-ins, pull nodes, windows and record terminals.
4. `RecordingSink` captures every emitted value upstream of the lossy outbox (C5).
5. Freeze three artifacts as the fixture:
   - `raw.arrows` — the canonical record batches the normalizer produced,
   - `expected.arrows` — the recorded DAG output,
   - `graphs.json` — the exact subscriptions, with their event-time window.

Fixture size is the practical constraint. A few minutes of a handful of symbols, ZSTD
Parquet, should be low single-digit MB; if it is not, it goes to git-lfs or a fetch step,
not into the repo. **Q13**.

### 10.2 CI run (every build, no network, no services)

```
ctest -R gma_replay_parity
```

1. Load `raw.arrows` into a temp `ParquetHistoryBackend` (`mktemp -d` — the workspace's
   shared-scratchpad hazard is real and fixed paths collide between parallel sessions).
2. Build the graphs from `graphs.json` through the ordinary `TreeBuilder`. **Same code
   path as a WS subscribe** — a test-only builder would prove nothing.
3. Drive a `ReplaySource` in `Emit` mode over the fixture window, `threadPoolSize=1`.
4. `ThreadPool::drain()` — the deterministic barrier, not a sleep (C6, ENC-1340).
5. Diff `actual.arrows` against `expected.arrows` on
   `(request_key, symbol, event_time_ns, bucket_start_ms, value)`.
6. Fail with the **first** differing row plus its neighbourhood. A parity failure whose
   message is a count is a parity failure nobody debugs.

### 10.3 What makes it a gate rather than a decoration

An instrument that cannot fail proves nothing — the lesson the ENC-1289 expected-failure
machinery in `CMakeLists.txt` was built around. So:

- **Anti-vacuity assertion.** The test asserts `expected` is non-empty, covers more than
  one `bucket_start_ms`, and contains at least one value from a windowed DAG. Without
  this, a replay that emits nothing passes every row comparison.
- **A falsification case that must fail.** `gma_replay_parity_wallclock`, registered
  `WILL_FAIL TRUE`, runs the same fixture with the event-time driver *disabled* so the
  window nodes use their wall clock. It must mismatch. If it ever passes, the parity test
  is not measuring what it claims and the suite says so at the point of the change rather
  than six months later.
- **The mutation must be asserted to have applied.** The wall-clock case verifies the
  flag actually took effect before concluding anything from the mismatch — a no-op
  mutation reads exactly like a passing gate.

### 10.4 Known non-determinism to pin before trusting the diff

- Pull nodes not adjacent to a clock hop (C10). Either exclude such graphs from the
  fixture or record them as expected-divergent, explicitly.
- `Interval`-clocked graphs have no event-time analogue at all: an `Interval` is a
  wall-clock cadence by definition. Replaying one is meaningless unless `Interval` gains
  an event-time mode. → **Q10**.
- Any graph whose output depends on map iteration order. `Dispatcher::_listeners`'s inner
  container is a `std::map<field,…>`, so within one tick the order is deterministic
  (`ask` before `bid`); the outer is an `unordered_map` keyed by symbol. Single-symbol
  fixtures sidestep it; multi-symbol ones need the diff keyed by symbol, not by arrival.

---

## 11. Dependency budget

Every dependency here needs a justification and a measured size before it is vendored.
**The numbers below are estimates and must be measured on this toolchain — do not quote
them as facts.**

| dep | why | rough static cost | verdict |
|---|---|---|---|
| Arrow C++ (`arrow`) | the canonical in-memory and IPC format; the whole "no row-by-row" claim rests on it | tens of MB; much trimmable via `ARROW_*=OFF` options | required, trim aggressively |
| Parquet (`parquet`) | default on-disk format | moderate on top of Arrow | required for the default backend |
| DuckDB | embedded query over Parquet, returns Arrow, no server | large (amalgamation) | **justify or replace.** Arrow's own Dataset API may cover the needed predicate pushdown without it. → **Q14** |
| `clickhouse-cpp` | optional native transport | small | optional, only with the ClickHouse backend |
| chDB | third option | large | not chosen (Q8) |
| Databento client | could be avoided by decoding DBN directly | small–moderate | **Q15** |

Against "embedded or single-binary, nothing ops-heavy by default", the live risk is
Arrow+Parquet+DuckDB being a larger payload than the engine itself. Two mitigations worth
designing for now rather than retrofitting: build `gma_data` as a **separately linkable**
library so a live-only deployment can omit it entirely, and keep `IHistoryBackend` narrow
enough that a minimal non-DuckDB implementation is viable.

---

## 12. Open questions and decisions needed

Ordered by how much they block. Q1–Q4 block the first tickets.

**Q1 — `bucketStartMs` under event time.** Making windows event-time-driven changes
`bucketStartMs` from a wall-clock boundary to an event-time boundary. That is a
wire-visible change against `specs/2026-09-20-timestamps-on-the-wire/SPEC.md` D9/D10,
which pins it to the wall clock on purpose. Amend that SPEC, or carry both
(`bucketStartMs` wall-clock + a new `eventBucketStartMs`) and let clients choose?
I would amend: two time bases on one wire frame is how charts end up disagreeing with
themselves.

**Q2 — Where does Arrow stop?** Confirm that "Arrow end to end with no row-by-row
conversion at boundaries" means the *data layer* is Arrow end to end with a single
column-wise decode at the engine boundary — and not that `INode`/`StreamValue` become
columnar. The second reading is a rewrite of ~25 node types and is not costed here.

**Q3 — Warm-up emission.** Confirm warm-up suppresses output and replay emits it (C4,
§4.5). If warm-up should emit, every chart gets a lookback of partially-warm indicator
values on connect, and the client has to know to discard them.

**Q4 — Fixed-point scale.** Confirm a single global 1e-9 scale (DBN-compatible, no
per-row scale column) rather than per-symbol. This decides whether `OrderBookManager`'s
per-symbol tick stays purely a book concern.

**Q5 — Replay determinism scope.** Is "identical output" promised for *all* graphs, or
only for graphs that avoid the documented execution-time-sampling case (C10, ENC-1335)?
The honest answer today is the latter, and the parity fixture should say so out loud.

**Q6 — Book snapshot cadence.** Every N deltas, every N seconds of event time, or at
session boundaries only? This trades storage against replay seek cost and I have no
data on your working window sizes.

**Q7 — ClickHouse transport.** `clickhouse-cpp` native, or HTTP + `FORMAT ArrowStream`?
HTTP costs throughput and saves a dependency. Given the backend is optional, I lean HTTP.

**Q8 — chDB.** Carry it as a third backend for SQL-dialect parity with the ClickHouse
option, or drop it? I lean drop — two embedded engines is a dependency we would be
carrying for dialect convenience.

**Q9 — Gap representation.** Are gaps rows in the store (queryable, replayable,
auditable) or metrics only (cheaper, invisible to a backtest)? A backtest that cannot
see "there was no data here" will silently treat a gap as a flat market.

**Q10 — Watermark policy, and `Interval`.** How long does an event-time window wait past
its boundary for late data before closing — fixed lag, per-source, provider-declared?
And separately: does `Interval` get an event-time mode, or is it defined as live-only and
excluded from replay? It is a wall-clock cadence by construction.

**Q11 — Sovereignty or central hosting; equities or crypto.** The brief and the
workspace's locked model disagree (C11). This decides whether `ClickHouseHistoryBackend`
is a real target, and whether the canonical schema needs equity-specific fields at all.
I would answer this first — it is the only question here that can invalidate other
answers.

**Q12 — Crash recovery for the raw path.** Is "replay the Arrow IPC tail into the sink"
the recovery mechanism, and who runs it — the normalizer on startup, or an operator?

**Q13 — Fixture size and storage.** How large a parity fixture is acceptable in-repo
before it goes to git-lfs or a fetch step? And: feed-simulator as the fixture source, or
a recorded Databento window (realistic, but is the data redistributable — which depends
on Q11)?

**Q14 — Is DuckDB justified?** It is the largest single dependency in the default
deployment. Arrow's own Dataset API may cover the Parquet predicate pushdown
`IHistoryBackend` needs. Worth measuring before vendoring.

**Q15 — Databento client library or decode DBN directly?** The schema reference is DBN
either way (which is the right call — it is a well-designed canonical model). Taking
their client is faster; decoding DBN ourselves keeps the adapter boundary honest and the
dependency list shorter.

---

## 13. Ticket breakdown sketch

Not created yet — most depend on the answers above. Dependency order, one repo each,
≤5h each:

| # | ticket | depends on |
|---|---|---|
| 1 | `[GMA_V3] StreamValue carries event time; every node copies it through` | Q1 |
| 2 | `[GMA_V3] event-time driver for TumblingWindow` | 1, Q10 |
| 3 | `[GMA_V3] event-time driver for BucketTime` | 1, Q10 |
| 4 | `[GMA_V3] Dispatcher::onRecord — non-JSON ingress` | Q2 |
| 5 | `[GMA_V3] gma_data skeleton + canonical Arrow schema` | Q4 |
| 6 | `[GMA_V3] normalizer: Arrow IPC live tail + batched sink` | 5 |
| 7 | `[GMA_V3] ParquetHistoryBackend over DuckDB` | 5, Q14 |
| 8 | `[GMA_V3] LiveSource + ReplaySource as ingress kinds` | 4, 5, 6 |
| 9 | `[GMA_V3] warm-up emission gate` | Q3 |
| 10 | `[GMA_V3] SplicedSource state machine` | 8, 9 |
| 11 | `[GMA_V3] book snapshot persistence + requestSnapshotFn wiring` | 7, Q6 |
| 12 | `[GMA_V3] SinkNode + RecordingSink` | 5 |
| 13 | `[GMA_V3] replay-parity fixture + CI gate` | 10, 12, Q13 |
| 14 | `[GMA_V3] ClickHouseHistoryBackend` | 7, Q7, Q11 |
| 15 | `[GMA_V3] Databento provider adapter` | 5, Q15 |

That is more than one ticket's worth, so per the workspace guardrails it is a **Linear
project with a `specs/<date>-<slug>/SPEC.md`**, not a pile of loose tickets. This document
is the design; whether it moves to `specs/` as the formal proposal directory or stays in
`docs/` as repo documentation with a thin SPEC pointing at it is **Q16** — the guardrail
wants `specs/`, and the repo's own convention put `ARCHITECTURE.md` in `docs/`.
