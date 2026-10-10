-- ---------------------------------------------------------------------------
-- GMA_V3 canonical market-data schema — ClickHouse DDL
--
-- Design sketch (ENC-1478). Matches data/include/gma/data/Schema.hpp and
-- docs/data-layer.md §5 column for column. NOT APPLIED BY ANYTHING YET:
-- ClickHouseHistoryBackend::ensureSchema() will run this at startup.
--
-- Every statement is IF NOT EXISTS, so applying it twice is a no-op.
--
-- Three tables, not one. A unified event table needs every type's fields
-- nullable, which defeats the codecs below and makes every read scan columns it
-- does not want.
--
-- DECISIONS ENCODED HERE
--
--   ts_event is DateTime64(9, 'UTC') — nanosecond event time, the ONLY time any
--   operator keys off. ts_recv is the normalizer's receipt time and is
--   diagnostics-only; it must never appear in a window boundary or a backtest.
--
--   Prices are Int64 at a FIXED GLOBAL scale of 1e-9 (gma::data::kPriceScale),
--   matching Databento DBN. There is deliberately no per-row scale column and
--   no per-symbol scale: OrderBookManager's per-symbol tick size stays a
--   book-quantization concern (docs C8 / Q4). A price of 1.234567890 is stored
--   as 1234567890.
--
--   ORDER BY (symbol, ts_event, provider_seq) on every table. It matches the
--   only read pattern there is — "this symbol, this time range" — and it
--   matches the Parquet backend's within-file sort, so neither backend has to
--   sort to answer a query.
--
--   CAVEAT, and it is a real cost: with symbol leading the key, a batch
--   spanning symbols is NOT inserted in primary-key order, so MergeTree sorts
--   each part on insert. That is the correct trade for a read-heavy store, but
--   batches should be symbol-sorted before insert to keep part merges cheap
--   (ParquetHistoryBackend::Config::sortBySymbolOnAppend does the same thing
--   for the same reason).
--
--   PARTITION BY toYYYYMMDD(ts_event) — day partitions. Mirrors the Parquet
--   layout's date= directory level, so a day's retention drop is one DROP
--   PARTITION on either backend.
--
--   CODECS. DoubleDelta on monotonic columns (ts_event, ts_recv, provider_seq):
--   second-order differences of a near-linear series are near zero.
--   Delta(8) + ZSTD(3) on prices: consecutive prices for one symbol differ by a
--   tick or two, so the delta is small and highly compressible. T64 + ZSTD(1)
--   on sizes: sizes are small integers in a narrow range, which is exactly what
--   T64's bit transposition is for.
--
--   dedup key is the PAIR (session_id, provider_seq), never the bare seq. A
--   normalizer restart resets a SYNTHESIZED provider_seq to 0, so comparing
--   bare seqs across a restart silently re-admits replayed rows
--   (docs §4.4, §8.4).
-- ---------------------------------------------------------------------------

CREATE DATABASE IF NOT EXISTS gma;

-- ---------------------------------------------------------------------------
-- trades (docs §5.2)
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.trades
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    venue         LowCardinality(String),

    price         Int64                 CODEC(Delta(8), ZSTD(3)),
    size          UInt64                CODEC(T64, ZSTD(1)),
    -- gma::Aggressor: 0 Unknown, 1 Buy, 2 Sell
    aggressor     UInt8                 CODEC(ZSTD(1)),
    -- Mirrors feed::ObTradeEvent::bookNeutral. TRUE means this print is a
    -- TA/print signal ONLY and must not consume resting liquidity — hidden /
    -- non-displayable executions, and executions whose book effect already
    -- arrived as an explicit add/update/delete. Dropping this column
    -- double-counts volume against the book.
    book_neutral  UInt8                 CODEC(ZSTD(1)),
    trade_id      UInt64                CODEC(ZSTD(1)),
    flags         UInt16                CODEC(ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- quotes — L1 top-of-book (docs §5.3)
--
-- One row per TOB CHANGE, not per inbound tick: a feed that repeats an
-- unchanged quote should not grow this table.
--
-- An absent side is gma::data::kNoPrice (INT64_MIN), not 0 and not NULL. 0 is a
-- legal price and negative prices are legal too
-- (OrderBookManager::setAllowNegativePrices exists for negative-yield bonds),
-- so neither can carry "absent"; and a Nullable column would cost a mask on the
-- hottest column in the table.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.quotes
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    venue         LowCardinality(String),

    bid_px        Int64                 CODEC(Delta(8), ZSTD(3)),
    ask_px        Int64                 CODEC(Delta(8), ZSTD(3)),
    bid_sz        UInt64                CODEC(T64, ZSTD(1)),
    ask_sz        UInt64                CODEC(T64, ZSTD(1)),
    bid_ct        UInt32                CODEC(T64, ZSTD(1)),
    ask_ct        UInt32                CODEC(T64, ZSTD(1)),
    flags         UInt16                CODEC(ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- book_events — L2/L3 deltas (docs §5.4)
--
-- `action` mirrors the gma::feed::FeedEvent variant set one-for-one
-- (gma::data::BookAction): 0 add, 1 update, 2 delete, 3 trade, 4 ticksize,
-- 5 clear. The repo already has a canonical book-event model; this does not
-- invent a second one.
--
-- `epoch` is FeedScope::epoch, bumped by a clear. It is in the row rather than
-- implied because OrderKey already carries it, which is what stops a pre-reset
-- order being matched by a post-reset message.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.book_events
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
    venue         LowCardinality(String),

    action        UInt8                 CODEC(ZSTD(1)),
    -- gma::Side: 0 Bid, 1 Ask
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
    flags         UInt16                CODEC(ZSTD(1))
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event, provider_seq)
SETTINGS index_granularity = 8192;

-- ---------------------------------------------------------------------------
-- book_snapshots (docs §5.4, §4.6)
--
-- Why this table exists: a replay of an arbitrary window has to seed the book
-- at the start of that window. Without a persisted snapshot that means
-- replaying every delta from session open — O(session) per backtest. With one,
-- IHistoryBackend::latestSnapshotAtOrBefore(symbol, ts) is a point lookup.
--
-- Cadence (every N deltas / N seconds of event time / session boundaries only)
-- is an open decision — docs Q6. It trades storage against replay seek cost.
--
-- Levels are nested rather than one row per level so a snapshot is one row and
-- therefore atomic: a half-written snapshot that reads as complete would seed a
-- torn book, which is worse than no snapshot at all.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.book_snapshots
(
    ts_event      DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    ts_recv       DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    provider_seq  UInt64                CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
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
-- gaps (docs §8.2, OPEN — Q9)
--
-- Whether gaps are rows or metrics only is undecided. This table is the "rows"
-- answer and it is the one I lean to: a backtest that cannot see "there was no
-- data here" will silently treat a gap as a flat market, and that is a
-- wrong-answer failure rather than a missing-telemetry one.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS gma.gaps
(
    ts_detected   DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    session_id    UInt32                CODEC(DoubleDelta, ZSTD(1)),
    symbol        LowCardinality(String),
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
