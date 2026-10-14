-- ---------------------------------------------------------------------------
-- GMA_V3 canonical market-data schema — ClickHouse L1 DDL
--
-- Design: specs/2026-10-14-gma-data-layer/SPEC.md (D2, D4, D5, D6, D7, D8, D11)
-- and docs/data-layer.md §8 (revision 3). Column names and types mirror
-- gma::data (include/gma/data/Record.hpp) one for one.
--
-- Apply it with data/schema/apply.sh. Every statement is IF NOT EXISTS, so
-- applying it twice is a no-op; apply.sh proves that by doing exactly that.
-- Nothing in the C++ build applies this yet — the ingest job (ENC-1483) is the
-- first caller.
--
-- =========================================================================
-- THIS IS AN L1 RAW STORE. IT COMPUTES NOTHING.
-- =========================================================================
--
-- SPEC D1/D2 govern: ClickHouse is indexed, compressed, columnar STORAGE AND
-- SELECTION, and gma performs 100% of the computation through one
-- implementation of every operator. A transpiler that reimplemented gma's math
-- in SQL would differ in float accumulation order, bucket edge inclusivity, EMA
-- seeding and gap handling — and this system exists to read differences of that
-- size as signal, so a discrepancy manufactured by the storage layer is
-- indistinguishable from an indication. There is no tolerance at which a second
-- implementation is safe, which is why the boundary is structural rather than a
-- parity diff (docs §1.1, §5.4).
--
-- D2 binds the DDL as well as the shim's SELECTs: the schema itself must not
-- compute. Consequently there is NOTHING in this file that aggregates, buckets
-- or derives:
--   * no aggregate function and no GROUP BY, anywhere
--   * no AggregatingMergeTree, no SummingMergeTree
--   * no MATERIALIZED VIEW producing bars, features or rollups
--   * no MATERIALIZED or ALIAS column computing a value
--   * no L2. No rollup table. No feature store. No bar table. (SPEC D4, docs §8.1)
-- Bars are produced by gma's TumblingWindow / BucketTime over the rows below.
--
-- `data/schema/check_no_compute.py` asserts all of that against this file at
-- any time, and `check_no_compute.py self-test` proves the assertion can fail.
-- NOTE FOR WHOEVER TOUCHES THAT CHECK: the prohibition list you just read
-- contains every string it looks for. It strips SQL comments before scanning
-- precisely so that documenting a prohibition is not reported as committing it,
-- and it has a fixture for that case. A naive grep would flag this header.
--
-- Three event tables, not one. A unified event table needs every type's fields
-- nullable, which defeats the codecs below and makes every read scan columns it
-- does not want.
--
-- DECISIONS ENCODED HERE
--
--   ts_event is DateTime64(9, 'UTC') — nanosecond event time, the ONLY time any
--   operator keys off (SPEC D9, docs §8.3). ts_recv is the normalizer's receipt
--   time and is diagnostics-only; it must never appear in a window boundary or
--   a backtest.
--
--   Prices are Int64 at a FIXED GLOBAL scale of 1e-9 (SPEC D8), which is
--   gma::data::kPriceScale == 1'000'000'000 in include/gma/data/Record.hpp.
--   The two must agree: the decode path is a reinterpret of the provider's own
--   fixed point and there is no conversion to catch a mismatch. There is
--   deliberately no per-row scale column and no per-symbol scale —
--   OrderBookManager's per-symbol tick size stays a book-quantization concern
--   (docs C9). A price of 1.234567890 is stored as 1234567890.
--
--   `symbol` IS THE TICKER, `instrument_id` IS PROVENANCE (SPEC D7, docs §8.9).
--   DBN's instrument_id is dataset-scoped and reassignable, while charts,
--   manifests and gma's streamKey all want "AAPL"; keying L1 on instrument_id
--   would make every query a symbology join and the store unreadable by hand.
--   instrument_id is kept on every event row because it is what the records
--   actually contain and it is how a row is traced back to the pull that
--   produced it. gma.symbology below holds the time-varying mapping.
--
--   ORDER BY (symbol, ts_event, provider_seq) on every event table. It matches
--   the only read pattern there is — "this symbol, this time range" — so a
--   query answers without a sort. provider_seq breaks ts_event ties
--   deterministically; it is monotonic within (session_id, symbol), which is
--   why session_id is on the row.
--
--   CAVEAT, and it is a real cost: with symbol leading the key, a batch
--   spanning symbols is NOT inserted in primary-key order, so MergeTree sorts
--   each part on insert. That is the correct trade for a read-heavy store, but
--   batches should be symbol-sorted per day partition before insert to keep
--   part merges cheap (the ingest job owns this; a background merge cannot fix
--   it after the fact — docs §8.6).
--
--   PARTITION BY toYYYYMMDD(ts_event) — day partitions, because the re-ingest
--   mechanism is DROP PARTITION (see below) and a trading day is the unit a
--   provider pull comes in. The three metadata tables (symbology,
--   ingest_windows) are unpartitioned: they are tiny and have no event time,
--   and gaps partitions on ts_detected because that is the only time it has.
--
--   CODECS (docs §8.10). DoubleDelta on monotonic columns (ts_event, ts_recv,
--   provider_seq): second-order differences of a near-linear series are near
--   zero. Delta(8) + ZSTD(3) on prices: MBO prices are highly repetitive, many
--   orders resting at one price. T64 + ZSTD(1) on sizes: small integers in a
--   narrow range, which is what T64's bit transposition is for. Delta(8) +
--   ZSTD(1) on order_id and priority: order_id is the high-cardinality column
--   that will dominate the table. Worth confirming against a real DBN file in
--   T5/ENC-1482 rather than taking this paragraph's word for it.
--
--   RE-INGEST IS `DROP PARTITION` + RELOAD, NOT DEDUP (SPEC D5/D6, docs §8.6).
--   Plain MergeTree on every event table. ReplacingMergeTree was revision 2's
--   answer and is REFUSED, because its dedup is exact only where the sort key
--   is unique:
--
--     for `trades`, (symbol, ts_event, provider_seq) is unique if the
--     provider's seq is per-symbol monotonic;
--
--     FOR `book_events` IT IS NOT. One DBN message can yield several records —
--     an order-executed produces a book mutation AND a trade print — so two
--     distinct rows can share the triple, and ReplacingMergeTree would SILENTLY
--     COLLAPSE THEM on an ordinary background merge. That deletes real data in
--     a way indistinguishable from a provider gap, which is the one failure
--     this layer cannot tolerate.
--
--   Dropping dedup dissolves that hazard outright (docs C12) and avoids merge
--   amplification on an MBO-sized table. What replaces it: re-pulling a window
--   is DROP PARTITION + reload from the RETAINED raw DBN files (SPEC D6 — the
--   files are load-bearing, not hygiene: the reload must reload from
--   something, and re-pulling from Databento spends the plan allowance this
--   architecture exists to work around), with gma.ingest_windows as the
--   authority on what is loaded. Pick one mechanism or the other, never both: a
--   ReplacingMergeTree with a non-unique key collapses rows no matter how you
--   re-ingest. If the DBN files are ever not retained, a ReplacingMergeTree
--   with a PROVABLY unique book_events key becomes mandatory and C12 returns.
--
--   Two tables are ReplacingMergeTree, and each says why at its definition:
--   gma.symbology and gma.ingest_windows. Both are tiny metadata tables with a
--   key that is unique by construction, and for both the point of the version
--   column is that an UPDATE to an existing row (closing a half-open interval;
--   an in-flight window becoming complete) is a replace rather than two
--   contradictory rows.
-- ---------------------------------------------------------------------------

CREATE DATABASE IF NOT EXISTS gma;

-- ---------------------------------------------------------------------------
-- trades (docs §8.4)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.trades
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    -- The DBN id, retained for provenance — docs §8.9. Not in the sort key.
    instrument_id UInt32                CODEC(T64, ZSTD(1)),
    venue         LowCardinality(String),

    price         Int64                 CODEC(Delta(8), ZSTD(3)),
    size          UInt64                CODEC(T64, ZSTD(1)),
    -- gma::data::Aggressor: 0 Unknown, 1 Buy, 2 Sell
    aggressor     UInt8                 CODEC(ZSTD(1)),
    -- Mirrors feed::ObTradeEvent::bookNeutral. TRUE means this print is a
    -- TA/print signal ONLY and must not consume resting liquidity — hidden /
    -- non-displayable executions, and executions whose book effect already
    -- arrived as an explicit add/update/delete. Dropping this column
    -- double-counts volume against the book.
    book_neutral  UInt8                 CODEC(ZSTD(1)),
    trade_id      UInt64                CODEC(ZSTD(1)),
    flags         UInt16                CODEC(ZSTD(1)),

    -- Skip index, not a projection: it stores a min/max per granule rather
    -- than a second copy of the data, so it costs no write amplification. It
    -- helps the cross-sectional read ("this ten-minute window, all symbols"),
    -- which the symbol-leading key does not serve.
    INDEX ix_ts ts_event TYPE minmax GRANULARITY 4
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- quotes — L1 top-of-book — DEFINED BUT NOT CREATED (SPEC Non-goals, docs §8.4)
--
-- DELIBERATELY COMMENTED OUT. There is no mbp-1 in the Databento plan, so
-- nothing populates this table: top-of-book is computed from the RECONSTRUCTED
-- book via OrderBookManager::bestBid/bestAsk, which is correct under SPEC D1
-- (one implementation of every operator) and consistent with ob.* being
-- pipeline-only (ENC-94/101). An empty table invites a reader to conclude the
-- market had no quotes, which is the §8.8 failure in miniature.
--
-- It stays here, in full, because feed-simulator and any future L1-only
-- provider do have top-of-book and nothing else, and re-deriving these columns
-- from scratch later is wasted work. Uncommenting it is the whole change.
--
-- One row per TOB CHANGE, not per inbound tick: a feed that repeats an
-- unchanged quote must not grow this table.
--
-- An absent side is gma::data::kNoPrice (INT64_MIN), not 0 and not NULL. 0 is a
-- legal price and negative prices are legal too
-- (OrderBookManager::setAllowNegativePrices exists for negative-yield bonds),
-- so neither can carry "absent"; and a Nullable column would cost a mask on
-- the hottest column in the table.
--
-- CREATE TABLE IF NOT EXISTS gma.quotes
-- (
--     ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
--     ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
--     provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
--     session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
--     symbol        LowCardinality(String),
--     instrument_id UInt32                CODEC(T64, ZSTD(1)),
--     venue         LowCardinality(String),
--
--     bid_px        Int64                 CODEC(Delta(8), ZSTD(3)),
--     ask_px        Int64                 CODEC(Delta(8), ZSTD(3)),
--     bid_sz        UInt64                CODEC(T64, ZSTD(1)),
--     ask_sz        UInt64                CODEC(T64, ZSTD(1)),
--     bid_ct        UInt32                CODEC(T64, ZSTD(1)),
--     ask_ct        UInt32                CODEC(T64, ZSTD(1)),
--     flags         UInt16                CODEC(ZSTD(1)),
--
--     INDEX ix_ts ts_event TYPE minmax GRANULARITY 4
-- )
-- ENGINE = MergeTree
-- PARTITION BY toYYYYMMDD(ts_event)
-- ORDER BY (symbol, ts_event, provider_seq)
-- SETTINGS index_granularity = 8192;
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- book_events — MBO / L3 deltas (docs §8.4)
--
-- `action` mirrors the gma::feed::FeedEvent variant set one-for-one
-- (gma::data::BookAction): 0 add, 1 update, 2 delete, 3 trade, 4 ticksize,
-- 5 clear. The repo already has a canonical book-event model; this does not
-- invent a second one.
--
-- `epoch` is FeedScope::epoch, bumped by a clear. It is in the row rather than
-- implied because OrderKey already carries it, which is what stops a pre-reset
-- order being matched by a post-reset message.
--
-- SORT KEY: THE PLAIN TRIPLE, AND THAT IS A DECISION (SPEC D5, docs §8.6).
-- Revision 2 ordered by (symbol, ts_event, provider_seq, action, order_id) to
-- make the key unique, because ReplacingMergeTree's correctness depended on
-- uniqueness. Dropping dedup dissolves that requirement: nothing collapses
-- rows, so a duplicated key is no longer a correctness hazard. Whether action
-- and order_id belong in the key is now purely a granule-skipping question —
-- do they help a read, and do they cost insert-time sorting — and ENC-1484 (T6)
-- measures sort-key uniqueness and that trade against real MBO.
--
-- Shipping the discriminated key here would pre-decide the question ENC-1484
-- exists to answer, so the committed baseline is the plain triple, matching
-- §8.6's stated DDL. Adding the two columns back is a one-line change IF T6
-- says they earn their keep.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.book_events
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    instrument_id UInt32                CODEC(T64, ZSTD(1)),
    venue         LowCardinality(String),

    action        UInt8                 CODEC(ZSTD(1)),
    -- gma::data::Side: 0 Bid, 1 Ask
    side          UInt8                 CODEC(ZSTD(1)),
    price         Int64                 CODEC(Delta(8), ZSTD(3)),
    size          UInt64                CODEC(T64, ZSTD(1)),
    -- 0 == the venue supplied no order id (OrderKey idMissing / the
    -- venue-key resolver path).
    order_id      UInt64                CODEC(Delta(8), ZSTD(1)),
    -- Order::priority — recv seq or venue ts.
    priority      UInt64                CODEC(Delta(8), ZSTD(1)),
    -- Level index for AGGREGATED (L2) feeds; 0 for per-order (L3).
    depth         UInt16                CODEC(T64, ZSTD(1)),
    epoch         UInt32                CODEC(DoubleDelta, ZSTD(1)),
    flags         UInt16                CODEC(ZSTD(1)),

    INDEX ix_ts ts_event TYPE minmax GRANULARITY 4
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- book_snapshots (docs §8.5)
--
-- Why this table exists: with MBO, replaying an arbitrary window without a
-- seed means replaying EVERY order event from session open — a full trading
-- day of per-order messages, per research run. That is the dominant cost of
-- windowed research, so snapshots are required rather than argued.
-- IL1Store::latestSnapshotAtOrBefore(symbol, ts) turns it into a point lookup,
-- and onSnapshotPerOrder(symbol, orders, snapshotSeq) already takes the
-- resume seq.
--
-- A snapshot is materialized STATE, which brushes against "no derived layers"
-- (SPEC D4). The distinction is that it is raw book state, not an aggregate:
-- OrderBookManager is the single implementation of applying deltas either way,
-- so nothing here computes a value a second time.
--
-- Cadence — session open plus a fixed event-time interval — is settled in
-- principle and tuned against B3 (docs §8.5). It trades storage against replay
-- seek cost and is a parameter of the ingest job, not of this schema.
--
-- Levels are nested rather than one row per level so a snapshot is one row and
-- therefore atomic: a half-written snapshot that reads as complete would seed a
-- torn book, which is worse than no snapshot at all. ClickHouse flattens a
-- Nested column into parallel arrays (levels.side, levels.price, …) and applies
-- the column's CODEC to each of them.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.book_snapshots
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    instrument_id UInt32                CODEC(T64, ZSTD(1)),
    venue         LowCardinality(String),

    epoch         UInt32                CODEC(DoubleDelta, ZSTD(1)),
    -- Parallel arrays (ClickHouse Nested). Ordered best -> worst within a
    -- side, matching DepthSnapshot's bids/asks ordering.
    levels       Nested(
        side        UInt8,
        price       Int64,
        size        UInt64,
        order_count UInt32
    ) CODEC(ZSTD(3))
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 1024;   -- snapshots are wide and rare

-- ---------------------------------------------------------------------------
-- symbology — SYMBOL IDENTITY OVER TIME (SPEC D7, docs §8.9)
--
-- THE ONE TABLE IN THIS FILE THAT CANNOT BE RETROFITTED, which is the whole
-- reason it goes in now, at one symbol, where it holds a single row and costs
-- nothing. Once there are months of event rows keyed on a bare ticker with no
-- provenance, reconstructing what that ticker meant on a given day is
-- archaeology against a provider's historical reference data. Writing one row
-- per pull as it happens is free; reconstructing it later may be impossible.
--
-- The mapping is (dataset, instrument_id) -> raw_symbol over a half-open
-- validity interval [valid_from, valid_to). dataset scopes instrument_id
-- because DBN ids are dataset-scoped and reassignable.
--
-- source_ref is the DBN batch id / file name the mapping was read out of, so a
-- row traces back to the pull that produced it — the same provenance role
-- instrument_id plays on the event tables.
--
-- ENGINE: ReplacingMergeTree(ingested_at), and it is safe HERE for reasons
-- that do not generalise to the event tables:
--
--   * the key (dataset, instrument_id, valid_from) is unique BY CONSTRUCTION —
--     one instrument cannot have two different symbols from the same instant,
--     and unlike book_events there is no mechanism by which one source record
--     legitimately yields two rows sharing a key;
--   * the table is tiny, so there is no merge amplification to speak of;
--   * and the version column is the point rather than an afterthought. A
--     mapping is discovered OPEN (valid_to far-future) and CLOSED later, when
--     the instrument is reassigned. Closing it is an update of valid_to on an
--     unchanged key, i.e. exactly the case a version-less ReplacingMergeTree
--     resolves arbitrarily. ingested_at makes the later write win.
--
-- ingested_at is an addition to docs §8.9's sketch, which has no version
-- column. Recorded as a deviation on purpose: without it, "the interval got
-- closed" and "the interval is still open" are two rows with the same key and
-- the engine may keep either.
--
-- READ-PATH HAZARD, same shape as gma.ingest_windows below: a plain SELECT can
-- return the pre-merge stale row, so the open-ended valid_to can outlive its
-- closure. Resolve the version on read (FINAL, or an argMax over ingested_at)
-- — and see the ingest_windows note for why doing so is not a D2 violation.
--
-- NOT SOLVED, and named rather than hidden: a ticker delisted and later
-- reassigned to a different company. valid_from/valid_to lets you DETECT it;
-- what a continuous series should do about it is reference data and out of
-- scope (SPEC Non-goals).
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.symbology
(
    dataset       LowCardinality(String),   -- e.g. 'XNAS.ITCH'
    instrument_id UInt32,
    raw_symbol    LowCardinality(String),   -- the ticker the event tables key on
    -- Half-open [valid_from, valid_to). An open interval carries valid_to at
    -- the far-future sentinel rather than NULL, so a range predicate needs no
    -- "what if absent" branch.
    valid_from    DateTime64(9, 'UTC'),
    valid_to      DateTime64(9, 'UTC'),
    -- Provenance: the DBN batch id / file name this mapping came from.
    source_ref    String,
    -- Version column — see the ENGINE note above.
    ingested_at   DateTime64(3, 'UTC')
)
ENGINE = ReplacingMergeTree(ingested_at)
ORDER BY (dataset, instrument_id, valid_from)
SETTINGS index_granularity = 1024;

-- ---------------------------------------------------------------------------
-- ingest_windows — INGEST COMPLETENESS (SPEC D11, docs §8.8)
--
-- The reason this table exists is that three situations are INDISTINGUISHABLE
-- in a plain SELECT, and the third silently returns a flat market:
--
--     the market was quiet         -> no seq discontinuity. This is data.
--     the provider has a hole      -> seq discontinuity. A `gaps` row.
--     WE HAVE NOT INGESTED IT YET  -> nothing at all, and no error anywhere.
--
-- A research run over a not-yet-ingested window must FAIL, not return a short
-- answer (IL1Store::isComplete). Completeness is RECORDED here by the ingest
-- job, NEVER INFERRED FROM ROW COUNTS — a quiet window and a missing window
-- have the same row count, which is the whole problem.
--
-- It also makes bulk ingest restartable: a ClickHouse insert is atomic per
-- block, not per batch, so an interrupted batch leaves some blocks committed,
-- and recovery is re-running the window — which DROP PARTITION + reload
-- (docs §8.6) makes exact.
--
-- ENGINE: ReplacingMergeTree(completed_at). The ticket for this schema named
-- symbology as "the one exception" to plain MergeTree; this is the second, and
-- the justification is the same three-part one:
--
--   * the key (symbol, kind, from_ts, session_id) is unique by construction —
--     one ingest session loads a given kind over a given window once;
--   * the table has one row per symbol-kind-window, so merge amplification is
--     not a consideration;
--   * and the version column is what makes a state transition a REPLACE rather
--     than two contradictory rows. The job writes state=0 (in-flight) and later
--     state=1 (complete) or state=2 (failed) for the SAME key. Without a
--     version, "in-flight" and "complete" coexist and the engine may keep
--     either. completed_at is zero on the in-flight row, so any real completion
--     wins.
--
-- ⚠ READ-PATH HAZARD — WRITE IT DOWN RATHER THAN DISCOVER IT. Replacement
-- happens at merge time, not at insert time, so between the two writes BOTH
-- rows exist and a plain SELECT may return either. The dangerous direction is
-- not the obvious one:
--
--     stale state=0 served while state=1 exists  -> a complete window reads as
--         incomplete. The query fails. Annoying; fail-safe.
--     stale state=1 served while state=0 exists  -> AN INCOMPLETE WINDOW READS
--         AS COMPLETE. This happens on every re-ingest: DROP PARTITION empties
--         the data while the old state=1 row is still in an unmerged part, so a
--         reader is told the window is loaded while the partition is empty, and
--         gets a flat market with no error. That is precisely the failure D11
--         exists to prevent.
--
-- So IL1Store::isComplete MUST resolve the version on read — FINAL, or an
-- argMax over completed_at per key — and never a bare SELECT.
--
-- AND THAT IS NOT A D2 VIOLATION, so do not "fix" it by removing the
-- resolution. D2 governs the MANIFEST -> SELECT path that feeds operators:
-- values that reach gma's DAG must arrive unaggregated, because a value
-- computed in SQL is a second implementation of gma's math. This read is
-- control-plane metadata — "is this window loaded", answered before any row is
-- selected — and its result is a boolean the planner acts on, not a number any
-- operator consumes. Resolving a version column here computes nothing about
-- the market.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.ingest_windows
(
    symbol        LowCardinality(String),
    -- 'trades' | 'book_events' | 'book_snapshots'  (and 'quotes' if that table
    -- is ever uncommented). One row per kind: the planner picks the table from
    -- the graph's needs, so kinds are loaded independently.
    kind          LowCardinality(String),
    from_ts       DateTime64(9, 'UTC'),
    to_ts         DateTime64(9, 'UTC'),     -- half-open [from_ts, to_ts)
    session_id    UInt32,
    -- 0 in-flight, 1 complete, 2 failed. A window is only readable at 1.
    state         UInt8,
    -- Diagnostics ONLY. A row count can never establish completeness (see
    -- above); `state` is the authority.
    rows_inserted UInt64,
    -- Version column — see the ENGINE note above. Zero while in-flight.
    completed_at  DateTime64(3, 'UTC'),
    -- Provenance, so a window can be re-pulled identically.
    source        LowCardinality(String),   -- 'databento' | 'feedsim' | ...
    source_ref    String                    -- DBN batch id / file name
)
ENGINE = ReplacingMergeTree(completed_at)
ORDER BY (symbol, kind, from_ts, session_id)
SETTINGS index_granularity = 1024;

-- ---------------------------------------------------------------------------
-- gaps — DISCONTINUITIES AS ROWS, NOT METRICS (docs §8.4)
--
-- Settled, not open: a research run that cannot see "there was no data here"
-- treats a gap as a flat market. That is a wrong-answer failure, not a
-- missing-telemetry one, so it belongs in the store a query can join against
-- rather than in a metrics backend a query cannot.
--
-- No ts_event and no provider_seq of its own, so this table cannot carry the
-- common sort key: it is keyed on when the discontinuity was DETECTED and the
-- seq range it covers.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.gaps
(
    ts_detected   DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    instrument_id UInt32                CODEC(T64, ZSTD(1)),
    venue         LowCardinality(String),
    -- Inclusive bounds of the missing run, in provider_seq.
    seq_from      UInt64                CODEC(DoubleDelta, ZSTD(1)),
    seq_to        UInt64                CODEC(DoubleDelta, ZSTD(1)),
    -- 0 seq-gap, 1 disconnect, 2 partial-frame-at-disconnect, 3 book reset
    cause         UInt8                 CODEC(ZSTD(1)),
    -- 0 none, 1 re-snapshot requested, 2 backfilled from history
    recovery      UInt8                 CODEC(ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_detected)
ORDER BY (symbol, ts_detected, seq_from)
SETTINGS index_granularity = 1024;

-- ---------------------------------------------------------------------------
-- PROJECTIONS — ALLOWED, NOT A LAYER, AND DEFERRED (SPEC D4, docs §8.2)
--
-- DELIBERATELY COMMENTED OUT. A projection is same table, same columns, no new
-- semantics: a second physical sort order maintained on insert and chosen by
-- the optimizer. That is an index, not a derived model, so it does not violate
-- D1 the way a rollup would — and it is genuinely the right tool for the
-- cross-sectional read, because the primary key leads with `symbol`:
--
--     replay / chart fill / sharded research  ->  (symbol, ts_event, provider_seq)
--     cross-sectional research                ->  (ts_event, symbol)
--
-- D4 defers them anyway, and the reason is arithmetic rather than taste: at ONE
-- symbol the two orderings are the same ordering, so a projection buys nothing
-- — while a second physical sort order maintained on insert is real write
-- amplification on an MBO-sized table, which is the one table whose insert rate
-- the B1/B2 measurement is about. Paying that for no benefit would also tilt
-- the measurement the whole project is sized by.
--
-- The minmax skip indexes on ts_event, which ARE committed above, are the cheap
-- half of the same idea: a min/max per granule instead of a second copy.
--
-- Enabling this when the universe grows is uncommenting it:
--
-- ALTER TABLE gma.trades      ADD PROJECTION IF NOT EXISTS p_time_symbol
--   ( SELECT * ORDER BY (ts_event, symbol) );
-- ALTER TABLE gma.book_events ADD PROJECTION IF NOT EXISTS p_time_symbol
--   ( SELECT * ORDER BY (ts_event, symbol) );
--
-- and then MATERIALIZE PROJECTION on each, which rewrites every existing part.
-- Do it per partition on a populated store.
-- ---------------------------------------------------------------------------
