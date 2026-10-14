-- Fixture: the derived bar layer this whole design refuses. Expected: REJECT.
-- Aggregates, a GROUP BY, time bucketing and a view, all in one statement —
-- i.e. SPEC D1, D2 and D4 at once. It is here because it is the thing someone
-- will eventually be tempted to add "just for chart fill".
CREATE MATERIALIZED VIEW IF NOT EXISTS fixture.bars_1m
ENGINE = MergeTree
ORDER BY (symbol, bucket)
AS SELECT
    toStartOfMinute(ts_event) AS bucket,
    symbol,
    sum(size)   AS volume,
    min(price)  AS low,
    max(price)  AS high
FROM fixture.trades
GROUP BY symbol, bucket;
