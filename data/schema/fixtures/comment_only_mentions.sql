-- Fixture: EVERY forbidden construct appears in this file, and every one of
-- them is inside a comment. Expected verdict: ACCEPT.
--
-- This is the shape of the real schema's header, reduced to its trap. The
-- prohibitions it documents are:
--   * no aggregate function: no sum(), no count(), no avg(x), no argMax(a, b),
--     no quantile(0.5)(x), no sumIf(x, y), no groupArray(x)
--   * no GROUP BY
--   * no AggregatingMergeTree, no SummingMergeTree
--   * no MATERIALIZED or ALIAS column computing a value
--   * no MATERIALIZED VIEW producing bars
--   * no window function: nothing with OVER (PARTITION BY symbol)
--
/* A block comment, because the stripper has to handle both forms:
   ENGINE = SummingMergeTree, a MATERIALIZED column `mid Int64 MATERIALIZED
   (bid_px + ask_px) / 2`, and
   CREATE MATERIALIZED VIEW gma.bars AS SELECT sum(size) FROM gma.trades
   GROUP BY toStartOfMinute(ts_event);
   None of this is SQL. All of it is prose about SQL. */

CREATE TABLE IF NOT EXISTS fixture.clean
(
    ts_event  DateTime64(9, 'UTC')  CODEC(DoubleDelta, ZSTD(1)),
    symbol    LowCardinality(String),
    price     Int64                 CODEC(Delta(8), ZSTD(3)),
    size      UInt64                CODEC(T64, ZSTD(1))  -- no aggregate here either
)
ENGINE = MergeTree
PARTITION BY toYYYYMMDD(ts_event)
ORDER BY (symbol, ts_event)
SETTINGS index_granularity = 8192;
