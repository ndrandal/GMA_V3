# GMA_V3 Data Layer — L1 ClickHouse, gma as the only compute

**Design reference:** [`docs/data-layer.md`](../../docs/data-layer.md) (revision 3).
**Origin ticket:** ENC-1478. **Date:** 2026-10-14.

---

## 0. TL;DR

Raw Databento **trades + mbo** (L3 per-order), **one symbol, seven days**, lands in a
single-node self-hosted ClickHouse **L1** store. No derived layers. ClickHouse is indexed
columnar **storage and selection only**; gma performs **100% of the computation** through
one implementation of every operator, shared by live, replay, chart fill and headless
research. A **shim** derives the minimal lossless `SELECT` from a gma manifest and streams
column blocks into gma with no row materialization.

The first deliverable is a **measurement**, not a feature: whether gma can consume rows as
fast as ClickHouse delivers them (`B2 ≥ B1`). Everything else is sized by that number.

---

## 1. Why

gma today has no history, no persistence and no replay. Three separate needs converge on
one layer:

1. **Replay / backtest** — run a graph over a past window.
2. **Chart fill** — a DynaCharting view attaches and needs historical bars; no such path
   exists today (`customer-layer/src/api/history.ts` is pipeline-build history, not market
   data).
3. **Headless research** — gma as a query engine transforming tables for research
   processes, outside DynaCharting entirely.

And the provider is changing: off DTN IQFeed onto Databento, with Databento Plus (live
TotalView) out of budget, so historical bulk pull into our own store is the only route.

The obvious design — compile a gma graph to SQL and let ClickHouse execute it — **is
refused**, see D1.

---

## 2. Locked decisions

**D1 — ClickHouse selects, gma computes. One implementation of every operator.**
A transpiler producing SQL for gma's math yields small numerical differences even when
correct: float accumulation order, bucket edge inclusivity, EMA seeding, NaN/empty-bucket
handling, timestamp tie-breaking. This system's business is reading differences of that
size as **signal**, so a discrepancy manufactured by the storage layer is
indistinguishable from an indication. A diff-based parity gate cannot rescue it — two
engines never agree bit-for-bit on floating-point aggregation, so the gate needs a
tolerance, and the tolerance *is* the noise floor. `docs/data-layer.md` §1.

**D2 — Only lossless selection may be pushed down, and a lint enforces it.**
Permitted: `WHERE` on `symbol`/`ts_event`/`provider_seq`, partition pruning, column
projection, `ORDER BY` matching the physical PK, `LIMIT` on a raw page only. Forbidden
without exception: aggregates, window functions, `GROUP BY`, time bucketing, any
expression in the projection list, computed `MATERIALIZED`/`ALIAS` columns. Enforced by
`gma_shim_sql_lint` at build time over the 272-entry request corpus — a structural
guarantee on query *shape*, not a numerical diff. §1.2–1.3, §5.4.

**D3 — Performance comes from transport, never from semantics.**
gma's per-value costs (a `std::string` symbol copy, a virtual call per node, a
`ThreadPool::post` hop per non-strand node, an `AtomicStore` write under a `shared_mutex`,
a rapidjson DOM at ingress) dominate at MBO rates. Removing them is **bit-identical by
construction** — same functions, same order, same arithmetic. Reimplementing any operator's
math in a form that changes accumulation order is a second implementation and is forbidden
by D1. §6.

**D4 — No derived layers. Projections are allowed and are not a layer.**
No L2, no rollup, no bar table, no feature store: research is ad hoc with no stable access
pattern, so a derived layer is a bet on queries nobody has written, and any *computed*
layer violates D1. ClickHouse projections (same data, second physical sort order) are an
index, not a model — deferred as pointless at one symbol. §8.1–8.2.

**D5 — Plain `MergeTree`; re-ingest by `DROP PARTITION` + reload.**
Not `ReplacingMergeTree`: its dedup is exact only on a unique sort key, and `book_events`
is not unique (one message yields a book mutation *and* a print), so a background merge
could silently delete a real row in a way indistinguishable from a gap. Dropping dedup
dissolves that hazard **and** avoids merge amplification on an MBO-sized table.
`gma.ingest_windows` is the authority on what is loaded. §8.6.

**D6 — The raw DBN files are retained; L1 is a rebuildable projection of them.**
Load-bearing, not hygiene: D5's reload must reload *from something*, and re-pulling from
Databento spends the plan allowance this architecture exists to work around. It also makes
a schema change a replay rather than a re-purchase, and protects a single un-replicated
local disk. If the files are ever not retained, `ReplacingMergeTree` with a provably-unique
`book_events` key becomes mandatory. §8.7.

**D7 — `symbol` is the ticker; `instrument_id` is retained; `gma.symbology` holds the
mapping.** DBN's `instrument_id` is dataset-scoped and reassignable while charts, manifests
and gma's `streamKey` all want `"AAPL"`. Keying L1 on `instrument_id` would make every
query a join and the store unreadable by hand. The mapping table goes in **now, at one
symbol, because it costs nothing now and cannot be retrofitted.** Ticker reuse after a
delisting is detectable via `valid_from`/`valid_to` and otherwise out of scope. §8.9.

**D8 — Prices are `Int64` at a fixed global 1e-9 scale.** Matches DBN exactly: no per-row
scale column, no per-symbol lookup on decode, an exact integer path with no double round
trip. `OrderBookManager`'s per-symbol tick stays a book-quantization concern. §C9.

**D9 — Operators key off event time only, and the three clock nodes are wall-clock today.**
`StreamValue` has no event-time field; `BucketTime`/`TumblingWindow` use
`system_clock::now()` + `wait_until` and `Interval` uses `cv.wait_for`. A 10-minute window
replayed through `TumblingWindow(60s)` yields **one** bar — a different *number* of outputs,
so no tolerance recovers it. `StreamValue` gains `eventTimeNs`; the window nodes gain
event-time drivers; the wall-clock thread survives in live mode only. `Interval` is defined
**live-only** and excluded from replay and research. §C1, §14.2.

**D10 — Warm-up depth is derived from the graph, and emission is one rule.**
The planner computes `effective = requested − warmup` by walking the graph for the deepest
history requirement, then **emit iff `ts_event >= requested.fromNs`**. A human setting
warm-up per request gets it wrong silently, in the direction of too little, producing output
that looks plausible and is warm-up garbage. §5.1, §C4.

**D11 — Ingest completeness is recorded, never inferred; an incomplete window fails.**
"Quiet market", "provider hole" and "not ingested yet" are indistinguishable in a plain
`SELECT`, and the third silently returns a flat market. `gma.ingest_windows` is written by
the ingest job and `IL1Store::isComplete` makes a query over an incomplete window **fail**
rather than answer short. Row counts cannot substitute: a quiet window and a missing window
have the same count. §8.8.

**D12 — trades + MBO puts every execution in the store twice; the planner refuses both.**
An execution is a `trades` row *and* MBO trade/fill records — two views, not duplicates, but
a graph reading both double-counts volume. `ObTradeEvent::bookNeutral` is the existing
live-path precedent. The planner already derives which `kinds` a graph touches and must
**refuse** a graph reading `trades` and `book_events` for the same symbol and window.
Reconciliation is a semantic choice masquerading as a convenience. §C13, Q15.

**D13 — Measurement precedes construction.** `B1` (rows/sec ClickHouse → gma, transport
only) and `B2` (rows/sec through the DAG) run before the shim, event time or any §6 change
is built. Target `B2 ≥ B1`. Neither needs event time, because a raw-scan benchmark uses a
non-windowed graph. Confound to design around: gma and ClickHouse share one machine and
compete for cores and page cache, so `B2 ≥ B1` can be satisfied for the wrong reason —
pinned cores or a sequential protocol, and a capped `max_server_memory_usage`. §14.

**D14 — Two deployment profiles, so ClickHouse being required is not a contradiction.**
Research/ingest runs a single-node self-hosted ClickHouse by design; the client profile is
live-only or reads a central L1 over the wire with no local store. Parquet and DuckDB are
**dropped entirely** — DuckDB was the largest dependency in the design and existed only to
query Parquet. §C10, §15.

---

## 3. What changes in each repo

| repo | change | when |
|---|---|---|
| **workspace** | `tools/stack.sh up clickhouse` — rootless podman, single node, memory-capped | first slice (T1) |
| **GMA_V3** | `data/` → `gma_data` library: schema, Databento adapter, ingest, shim; `Dispatcher::onRecord`; benchmarks; later event time + `ReplaySource` + sinks | first slice + post-measurement |
| **embassy** | seed `baseMs` from the fill window; pre-size `createBuffer` | deferred (chart fill) |
| **treaty** | wire change if Q1 resolves toward a second time basis | deferred |
| **customer-layer** | chart-fill request surface | deferred |

Dependency direction is unchanged: `gma_data` depends on `gma_engine`; `gma_engine` depends
on neither it nor any connector. **Arrow and the ClickHouse client must never become engine
dependencies.**

---

## 4. Non-goals

- Any derived layer, rollup, bar table or feature store (D4).
- Transpiling gma graphs to SQL, in any form (D1).
- Vectorized or columnar re-implementation of any operator (D3).
- `SplicedSource`'s live half — nothing exercises it until Databento live exists.
- Live TotalView / Databento Plus.
- `quotes` (L1 top-of-book) population: no `mbp-1` in the plan; TOB comes from the
  reconstructed book.
- Sharding (§7) and projections (§8.2) — premature at one symbol.
- Chart fill (§10), including infinite scroll-back, which the current wire cannot express.
- Corporate actions, adjusted prices, symbol-change continuity.
- Retention, compaction, multi-provider merge.
- Parquet and DuckDB.

---

## 5. Open questions

Resolve before the ticket that depends on each. None block the first slice.

| id | question | blocks |
|---|---|---|
| **Q1** | `bucketStartMs` under event time: amend `specs/2026-09-20-timestamps-on-the-wire` D9/D10, or carry both bases on the wire? *Recommend amend.* | the wire half of event time |
| **Q5** | Is bit-identical output promised for *all* graphs, or only those avoiding C11's execution-time-sampling case? *Honest answer: the latter.* | replay parity fixture |
| **Q7** | Cross-symbol graphs (`by:"none"`, ENC-1292) cannot shard. Unsharded, or symbol-group shard, and who picks the group? | sharding |
| **Q8** | Long-interval fill: accept the raw scan, or one narrow exception to D4? | after B3 |
| **Q10** | Chart fill depth at scene-init: accept bounded attach-with-N-bars, or change the wire? *Recommend accept.* | embassy ticket |
| **Q11** | Persist derived series as a cache? If yes, invalidation key is graph identity. | after B3 |
| **Q14** | Databento client library, or decode DBN directly? | decided in passing by T4 |
| **Q15** | Confirm the planner **refuses** a graph reading `trades` + `book_events` for one symbol/window (D12) rather than reconciling. *Recommend refuse.* | the planner |

**Unverified facts, to be established by measurement rather than assertion:**
exact DBN field names and whether `instrument_id` is stable within a dataset-day (T5);
`book_events` sort-key uniqueness against real MBO (T6); whether `ob::Provider` consults
`isStale` before serving `ob.*` (post-measurement).

---

## 6. Ticket breakdown

Nine tickets to a measured answer. One repo each, ≤5h each, dependency-ordered.
The 21-ticket plan in revision 2 of the design doc is the **post-measurement** plan.

| # | id | title | repo | est | deps |
|---|---|---|---|---|---|
| T1 | ENC-1479 | `[workspace] tools/stack.sh up clickhouse` — rootless podman single-node L1 | workspace | 3 | — |
| T2 | ENC-1480 | `[GMA_V3] L1 DDL` — trades, book_events, book_snapshots, symbology, ingest_windows, gaps | GMA_V3 | 3 | T1 |
| T3 | ENC-1481 | `[GMA_V3] gma_data skeleton` + canonical Schema.hpp + CMake target | GMA_V3 | 4 | — |
| T4 | ENC-1482 | `[GMA_V3] Databento DBN adapter` — trades + mbo → canonical batches | GMA_V3 | 5 | T3 |
| T5 | ENC-1483 | `[GMA_V3] ingest job` — 7d/1symbol → L1, retain DBN, completeness rows | GMA_V3 | 4 | T2, T4 |
| T6 | ENC-1484 | `[GMA_V3] measure book_events sort-key uniqueness` against real MBO | GMA_V3 | 2 | T5 |
| T7 | ENC-1485 | `[GMA_V3] B1 benchmark` — ClickHouse → gma transport, native vs HTTP | GMA_V3 | 4 | T5 |
| T8 | ENC-1486 | `[GMA_V3] Dispatcher::onRecord` — non-JSON ingress | GMA_V3 | 4 | T3 |
| T9 | ENC-1487 | `[GMA_V3] B2 benchmark` — rows/sec through the DAG | GMA_V3 | 5 | T7, T8 |

Critical path: T1 → T2 → T5 → T7 → T9. T3/T8 and T1/T2 parallelise.
