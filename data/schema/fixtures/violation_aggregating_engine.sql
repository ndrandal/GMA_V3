-- Fixture: an aggregating engine. Expected verdict: REJECT.
-- The engine computes during background merges, so by the time any SELECT runs
-- the value already exists and no SELECT lint can see where it came from.
CREATE TABLE IF NOT EXISTS fixture.bars_agg
(
    bucket    DateTime64(9, 'UTC'),
    symbol    LowCardinality(String),
    volume    AggregateFunction(sum, UInt64)
)
ENGINE = AggregatingMergeTree
ORDER BY (symbol, bucket);
