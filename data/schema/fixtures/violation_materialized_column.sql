-- Fixture: a MATERIALIZED column. Expected verdict: REJECT.
-- `mid` is gma's arithmetic, executed by ClickHouse at insert time. It is a
-- second implementation of an operator (SPEC D1) hidden in a column default.
CREATE TABLE IF NOT EXISTS fixture.quotes_mid
(
    ts_event  DateTime64(9, 'UTC'),
    symbol    LowCardinality(String),
    bid_px    Int64,
    ask_px    Int64,
    mid       Int64 MATERIALIZED intDiv(bid_px + ask_px, 2)
)
ENGINE = MergeTree
ORDER BY (symbol, ts_event);
