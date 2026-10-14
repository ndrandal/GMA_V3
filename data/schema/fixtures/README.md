# `check_no_compute.py` fixtures

A gate never observed failing has not been shown to gate. These files are what
`python3 data/schema/check_no_compute.py self-test` runs the D2 check over, so
every pass of the real schema comes with a demonstration that the same check
rejects the thing it claims to forbid.

| fixture | expected | what it pins down |
|---|---|---|
| `../clickhouse.sql` | **accept** | the real schema, including a header that names every forbidden construct in prose |
| `comment_only_mentions.sql` | **accept** | every forbidden construct appears, but only inside `--` and `/* */` comments. The comment stripper must make these invisible. |
| `violation_aggregating_engine.sql` | **REJECT** | `ENGINE = AggregatingMergeTree` |
| `violation_materialized_column.sql` | **REJECT** | a `MATERIALIZED` column computing a value |
| `violation_aggregate_view.sql` | **REJECT** | a bar-producing view: `sum()`, `min()`, `max()`, `GROUP BY`, `toStartOfMinute()` |
| `violation_alias_column.sql` | **REJECT** | an `ALIAS` column — computation at read time, invisible in the projection list |
| `vacuous_comments_only.sql` | **REJECT** | comments and nothing else. Proves the check fails on zero statements instead of reporting a clean scan of nothing. |

The accept/REJECT pair across `comment_only_mentions.sql` and the violation
files is the whole point: it proves the stripper works in **both** directions.
Stripping too little turns the schema's own documentation into a violation;
stripping too much (or scanning nothing at all) turns a real violation into a
pass, which is why `vacuous_comments_only.sql` is in the table.

Nothing here is ever applied to a server.
