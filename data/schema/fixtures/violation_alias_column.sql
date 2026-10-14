-- Fixture: an ALIAS column. Expected verdict: REJECT.
-- Worse than MATERIALIZED in one way: it is computed at READ time and never
-- appears in the projection list the shim's lint inspects, so `SELECT *`
-- quietly carries an expression gma did not evaluate.
CREATE TABLE IF NOT EXISTS fixture.trades_notional
(
    ts_event  DateTime64(9, 'UTC'),
    symbol    LowCardinality(String),
    price     Int64,
    size      UInt64,
    notional  Int64 ALIAS price * toInt64(size)
)
ENGINE = MergeTree
ORDER BY (symbol, ts_event);
