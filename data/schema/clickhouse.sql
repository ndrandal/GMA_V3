-- ---------------------------------------------------------------------------
-- GMA_V3 canonical market-data schema — ClickHouse L1 DDL
--
-- Design: specs/2026-10-14-gma-data-layer/SPEC.md (D2, D4, D5, D6, D7, D8, D11)
-- and docs/data-layer.md §8 (revision 3).
--
-- RELATIONSHIP TO THE C++ PODs, STATED EXACTLY, BECAUSE IT IS NOT ONE-FOR-ONE.
-- The engine-facing PODs in `namespace gma::data` (today
-- data/include/gma/data/Schema.hpp; moving to include/gma/data/Record.hpp with
-- ENC-1481) are a SUBSET of these columns, not a mirror of them:
--
--   * every POD field has a column here, with the same name in snake_case and a
--     type that round-trips without loss;
--   * these tables additionally carry STORAGE AND PROVENANCE columns no POD has
--     — `instrument_id` on trades / book_events / book_snapshots / gaps (D7:
--     retained provenance, deliberately not on the engine's value path), and
--     every column of symbology, ingest_windows and gaps, none of which the
--     engine ever sees;
--   * the one value the two MUST agree on numerically is the price scale —
--     see the next decision.
--
-- So: a POD field missing a column is a bug; a column with no POD field is
-- expected and is the normal case for anything the engine does not compute on.
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
-- One table per record type, not one unified event table. A union table needs
-- every type's fields nullable, which defeats the codecs below and makes every
-- read scan columns it does not want. (Two event tables are live here —
-- `trades` and `book_events` — because `quotes` is defined-but-commented; see
-- its block.)
--
-- DECISIONS ENCODED HERE
--
--   ts_event is DateTime64(9, 'UTC') — nanosecond event time, the ONLY time any
--   operator keys off (SPEC D9, docs §8.3). ts_recv is the normalizer's receipt
--   time and is diagnostics-only; it must never appear in a window boundary or
--   a backtest.
--
--   Prices are Int64 at a FIXED GLOBAL scale of 1e-9 (SPEC D8), which is
--   gma::data::kPriceScale == 1'000'000'000 (see the header note above for
--   which file that constant lives in). The two must agree: the decode path is a reinterpret of the provider's own
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
--   provider pull comes in. `gaps` partitions on toYYYYMMDD(ts_from), i.e. the
--   day of the HOLE, so that the same DROP PARTITION reaches it; see that
--   table for why the detection day was wrong. The two metadata tables
--   (symbology and ingest_windows) are unpartitioned: they are tiny and have
--   no event time of their own.
--
--   ⚠ AND THE PARTITION IS DAY-ONLY WHILE COMPLETENESS IS PER SYMBOL, which
--   makes a DROP PARTITION hit symbols nobody asked to re-ingest. Dropping day
--   D to reload symbol A also deletes symbol B's rows for D, and B's
--   completeness row still says state=1 — so isComplete(B, D) answers true over
--   an empty partition. The full invariant, and who owns it, is written at
--   gma.ingest_windows; docs §8.6 records the reload COST of day-granular
--   partitions but not this invalidation. Moot at one symbol, which is the
--   reason to write it down now instead of meeting it at the universe. The
--   finer-grained alternative docs §8.6 names (DELETE WHERE symbol = … AND
--   ts_event IN range, then insert) avoids the collateral damage entirely and
--   is the right move once per-symbol re-pulls are common; it is not the
--   committed mechanism today because at one symbol it buys nothing.
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
--   EXACTLY ONE TABLE IS ReplacingMergeTree: gma.symbology, and it says why at
--   its definition. Its key is unique by construction, the table is tiny, and
--   its version column is a WRITE clock (`ingested_at`), which is what makes
--   closing a half-open interval a replace rather than two contradictory rows.
--
--   gma.ingest_windows was ReplacingMergeTree(completed_at) in revision 2 and is
--   NOT any more. completed_at is zero while a window is in flight, so on a
--   re-ingest the engine resolves to the STALE complete row and then deletes
--   the in-flight one at merge time — an incomplete window reads as complete,
--   forever and silently, which is the one thing D11 exists to prevent. That is
--   measured, not reasoned; see the table's own comment. It is a plain-MergeTree
--   append-only transition log, with a read rule written down beside it.
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
-- ENGINE: PLAIN MergeTree, AND THIS TABLE IS AN APPEND-ONLY TRANSITION LOG.
-- One row per state transition of one window, never an update in place.
--
-- ⚠ THIS REVERSES THE REVISION-2 DDL, WHICH WAS
-- `ReplacingMergeTree(completed_at)`, AND THE REASON IS MEASURED RATHER THAN
-- ARGUED (ENC-1480, against the server tools/stack.sh runs). completed_at is a
-- COMPLETION clock, not a WRITE clock: it is zero while a window is in flight.
-- So on a re-ingest — the exact case this table exists to make safe — the
-- sequence is
--
--     write 1   state=1, completed_at=2026-10-02T01:00   (loaded)
--     write 2   state=0, completed_at=0                  (re-ingest starts;
--                                                         DROP PARTITION has
--                                                         already emptied the
--                                                         data)
--
-- and because 0 < 2026-10-02T01:00, ReplacingMergeTree keeps write 1. Observed:
-- a bare SELECT returns both rows and may serve either; `FINAL` and an argMax
-- over completed_at BOTH resolve to the stale state=1; and after
-- `OPTIMIZE TABLE … FINAL` the in-flight row is GONE and only state=1 survives.
-- That is not a transient read hazard — it is the engine permanently deleting
-- the row that says "this window is not loaded right now" and keeping the one
-- that says it is. The window then reads as complete forever, with no error,
-- and the query returns a flat market: precisely the failure D11 exists to
-- prevent.
--
-- Fixing the version column (a write clock instead of a completion clock) would
-- close that, but it still does not earn the exception, because THE READER
-- NEEDS VERSION RESOLUTION EITHER WAY — see the read rule below.
-- ReplacingMergeTree would then buy only storage compaction, on a table with a
-- handful of rows per symbol-day, in exchange for destroying the re-ingest
-- audit trail. So: plain MergeTree, per SPEC D5's default, and gma.symbology
-- stays the ONE exception in this file — its version column IS a write clock
-- (`ingested_at`), verified in the same run resolving an open interval's later
-- closure correctly.
--
-- ⚠ THE READ RULE, AND IT IS NOT OPTIONAL. Three separate ways a naive read of
-- this table answers "complete" over data that is not there, each measured:
--
--   1. `state = 1` ON SOME ROW IS NOT COMPLETENESS. A window accumulates rows
--      forever, including rows from sessions that WERE complete before a
--      re-ingest emptied the partition. Measured: after (session 1, state=1)
--      then (session 7, state=0), `WHERE state = 1` matches one row and the
--      window reads as complete while its data is gone.
--
--   2. RESOLVING PER RECORDED WINDOW IS NOT COVERAGE OF THE REQUEST. A recorded
--      [00:00, 12:00) at state=1 says nothing about [12:00, 24:00). A rule that
--      resolves the newest row for a (symbol, kind, from_ts) and stops there
--      answers "complete" for a request covering [00:00, 24:00) and returns
--      HALF A DAY AS IF WHOLE — the flat-market failure D11 exists to prevent,
--      in the one rule meant to prevent it. to_ts is therefore in the sort key
--      and in the rule, and the rule is about COVERAGE, not about a row.
--
--   3. A TIE IN updated_at MAKES "THE NEWEST ROW" ARBITRARY. See the column's
--      DEFAULT below.
--
-- So:
--
--     isComplete(symbol, kind, [req_from, req_to)) :=
--         W := { [from_ts, to_ts) recorded for (symbol, kind)
--                whose NEWEST row by updated_at has state = 1 }
--         require  [req_from, req_to)  is covered by  union(W).
--         Any part of the request not covered -> NOT INGESTED -> FAIL (D11).
--
-- Notes on that rule, because each clause is load-bearing:
--
--   * "newest row by updated_at" is resolved PER (symbol, kind, from_ts, to_ts)
--     — one recorded window — and only then do the surviving state=1 windows
--     form W. Resolving first and covering second is the order that matters: a
--     window re-opened by a re-ingest must drop OUT of W before coverage is
--     tested.
--   * union(W) so that a seven-day request over seven daily windows is
--     complete. Adjacent half-open intervals join; a hole anywhere fails.
--   * a state=0 / 2 / 3 newest row contributes nothing. There is no partial
--     credit, and `rows_inserted` is never consulted.
--   * THE COVERAGE TEST IS DONE BY THE CALLER over the selected rows, not in
--     SQL. That keeps it a selection on this side of the boundary and is also
--     simply easier to get right in C++ than in an interval-merge query.
--
-- SORT KEY: (symbol, kind, from_ts, to_ts, updated_at), and session_id is
-- DELIBERATELY NOT IN IT. With session_id sitting between the window prefix and
-- the clock, the tail of a prefix range is the newest row OF THE HIGHEST
-- session_id, not the newest row overall — and nothing makes session_id
-- time-monotonic (it exists precisely because a restart resets a synthesized
-- counter, as the provider_seq note in the header says). A reader taking the
-- tail would resolve to a stale state=1 from a higher-numbered OLDER session.
-- With session_id out of the key, "the newest row for this window" genuinely IS
-- the tail of the (symbol, kind, from_ts, to_ts) prefix range, and session_id
-- stays on the row as provenance. The key is not unique and does not need to
-- be: nothing dedups here, which is the entire point of plain MergeTree (D5).
--
-- ⚠ INVARIANT THAT THE SCHEMA CANNOT ENFORCE, SO IT IS WRITTEN HERE AND OWNED BY
-- THE INGEST JOB (ENC-1483):
--
--     A `DROP PARTITION` INVALIDATES EVERY SYMBOL'S COMPLETENESS ROWS FOR THAT
--     PARTITION, NOT JUST THE SYMBOL BEING RE-INGESTED.
--
-- Partitions are day-granular (`toYYYYMMDD(ts_event)`) while completeness is
-- recorded PER SYMBOL, so re-ingesting symbol A for a day deletes symbol B's
-- rows for that day too. Without this step B's newest row still reads state=1
-- and isComplete(B, …) returns true over an empty partition. docs §8.6 records
-- the reload COST of day-granular partitions; it does not record this
-- invalidation, and at one symbol the two are indistinguishable — which is
-- exactly why it has to be written down now rather than found at the universe.
-- Concretely, dropping partition P obliges the job to write, for every
-- (symbol, kind) with a recorded window intersecting P, a fresh row at
-- state = 3 (invalidated) or state = 0 (in flight, if it is about to reload
-- that symbol too). `gma.gaps` is partitioned to the same key so the same
-- DROP PARTITION removes its rows for P — see that table.
-- Nothing in ClickHouse checks any of this: there is no foreign key and no
-- trigger. It is a code obligation, and the ingest job's tests are where it
-- gets held up.
--
-- AND RESOLVING THE NEWEST ROW IS NOT A D2 VIOLATION, so do not "fix" it by
-- removing the resolution. D2 governs the MANIFEST -> SELECT path that feeds
-- operators: values that reach gma's DAG must arrive unaggregated, because a
-- value computed in SQL is a second implementation of gma's math (D1). This
-- read is control-plane metadata — "is this window loaded", answered before any
-- market row is selected — and its result is a boolean the planner acts on, not
-- a number any operator consumes. An ORDER BY … LIMIT 1, or an argMax over
-- updated_at, computes nothing about the market.
--
-- OPEN, and it belongs to the ingest job (ENC-1483) rather than to the schema:
-- whether a re-ingest reuses its window's session_id or takes a new one. The
-- read rule above is correct under both, which is why session_id is out of the
-- key and out of the rule. (docs §8.8 states the recorded-never-inferred rule
-- and the three-situations table; it specifies NO DDL and NO sort key, so the
-- key here — and its divergence from revision 2's — is this file's decision to
-- own, not a doc citation to lean on.)
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
    -- 0 in-flight, 1 complete, 2 failed, 3 invalidated (the data was removed by
    -- a partition reload for another symbol — see the invariant above). ONLY 3
    -- is new relative to revision 2, and it exists because "collaterally wiped"
    -- is neither in-flight nor failed, and writing it as either would be a lie
    -- in the one table whose job is to not lie. A window is readable only when
    -- its NEWEST row is state = 1.
    state         UInt8,
    -- Diagnostics ONLY. A row count can never establish completeness (see
    -- above); `state` is the authority.
    rows_inserted UInt64,
    -- WRITE CLOCK, and the DEFAULT is what makes "set on every row" true rather
    -- than a convention. Measured: with no DEFAULT, an INSERT that omits this
    -- column stores 1970-01-01 00:00:00.000 — so if ENC-1483 ever omits it,
    -- every row ties at epoch 0, "the single newest row" becomes arbitrary and a
    -- stale state=1 can win. That is the IDENTICAL zero-valued-clock defect this
    -- table was rewritten to eliminate; see the ENGINE note above.
    --
    -- `DEFAULT now64(3)` is not a D2 violation: a column default computes
    -- nothing about the market, it only fires when the column is omitted, and
    -- D2's prohibition is on MATERIALIZED / ALIAS columns deriving a VALUE. A
    -- wall clock on a control-plane metadata row is not an operator's input.
    updated_at    DateTime64(3, 'UTC') DEFAULT now64(3),
    -- Semantic: when this window finished loading. Zero on an in-flight,
    -- failed or invalidated row. NEVER use it to order rows — doing exactly
    -- that is what made the revision-2 engine delete the in-flight row.
    completed_at  DateTime64(3, 'UTC'),
    -- Provenance, not identity: which ingest session wrote this row. It is NOT
    -- in the sort key; see the SORT KEY note above.
    session_id    UInt32,
    source        LowCardinality(String),   -- 'databento' | 'feedsim' | ...
    source_ref    String                    -- DBN batch id / file name
)
ENGINE = MergeTree
ORDER BY (symbol, kind, from_ts, to_ts, updated_at)
SETTINGS index_granularity = 1024;

-- ---------------------------------------------------------------------------
-- gaps — DISCONTINUITIES AS ROWS, NOT METRICS (docs §8.4)
--
-- Settled, not open: a research run that cannot see "there was no data here"
-- treats a gap as a flat market. That is a wrong-answer failure, not a
-- missing-telemetry one, so it belongs in the store a query can join against
-- rather than in a metrics backend a query cannot.
--
-- ⚠ PARTITIONED ON THE DAY OF THE HOLE, NOT THE DAY IT WAS DETECTED, AND THAT
-- CHANGED UNDER ENC-1480. Revision 2 had `PARTITION BY toYYYYMMDD(ts_detected)`,
-- which is a different day from the data it describes — a hole in Monday's feed
-- can be detected on Tuesday, or on any later day by a re-scan. The consequence
-- is that the §8.6 re-ingest mechanism CANNOT REACH THESE ROWS: dropping the
-- event tables' partition for Monday leaves Monday's gap rows behind, so a
-- corrected re-pull that fills the hole ends with a row still asserting the
-- discontinuity, and a research run joining `gaps` sees a PHANTOM HOLE in data
-- that is now complete. That is the same class of wrong answer as the gap it
-- was recording.
--
-- Partitioning on `toYYYYMMDD(ts_from)` puts these rows on the same partition
-- key as the event tables, so re-ingesting a day drops its gap rows with the
-- rest of it and one `DROP PARTITION` per day is uniform across trades,
-- book_events, book_snapshots and gaps. The per-symbol collateral damage that
-- creates is covered by the invariant in gma.ingest_windows above — this table
-- is named there for exactly that reason.
--
-- The cost, recorded: the detector must supply event-time bounds for the hole,
-- not only seq bounds. It has them — a seq discontinuity is found between two
-- messages, and both carry ts_event — but it is an obligation on ENC-1483 that
-- revision 2 did not impose. ts_detected stays as a diagnostic column.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.gaps
(
    -- Event-time bounds of the missing run, half-open [ts_from, ts_to): the
    -- ts_event of the last message before the hole and of the first message
    -- after it. These are what the partition key and the sort key are built on.
    ts_from       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_to         DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    -- Diagnostics: when the detector noticed. May be a different day, which is
    -- the whole reason it is not the partition key.
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
PARTITION BY toYYYYMMDD(ts_from)
ORDER BY (symbol, ts_from, seq_from)
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
