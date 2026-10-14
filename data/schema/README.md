# `data/schema` — the L1 ClickHouse DDL and its gates

| file | what it is |
|---|---|
| `clickhouse.sql` | **the schema.** Six tables: `trades`, `book_events`, `book_snapshots`, `symbology`, `ingest_windows`, `gaps`. Every statement is `IF NOT EXISTS`. |
| `apply.sh` | the acceptance run: lint, self-tests, apply from scratch, apply twice more, `SHOW CREATE TABLE` comparison, and a demonstration that the comparison can fail. |
| `apply.py` | `apply` / `verify` / `prove-verify` / `emit` against a server over HTTP. |
| `check_no_compute.py` | the committed D2 check — the DDL must not compute — plus `self-test`. |
| `ddl.py` | the one DDL reader both of the above use. |
| `clickhouse_aggregates.txt` | the aggregate-function vocabulary, read off the server rather than recalled. |
| `fixtures/` | files the check is proven to reject, and one it is proven to accept. |

Design: `specs/2026-10-14-gma-data-layer/SPEC.md` (D2, D4, D5, D6, D7, D8, D11) and
`../../docs/data-layer.md` §8. Ticket: ENC-1480.

## Running it

```bash
# from the workspace root, once:
tools/stack.sh up clickhouse
tools/stack.sh doctor                 # exits non-zero if the server is not fit

# from this directory:
./apply.sh                            # idempotent apply + verify
./apply.sh --fresh                    # DROP DATABASE gma first — DESTRUCTIVE
python3 check_no_compute.py lint      # no server needed
python3 check_no_compute.py self-test # proves the lint can fail
```

`CH_HTTP_URL` overrides the endpoint (a worktree at `STACK_PORT_OFFSET=<n>` publishes
HTTP on `8123+<n>`).

## The two properties worth knowing

**The lint strips SQL comments before it scans, and that is not a convenience.** The
schema's own header documents every construct the lint forbids, by name. Scanning the
raw text reports the documentation as the violation — 7 occurrences, today. Scanning
nothing at all reports a clean pass. `self-test` pins both ends: a file whose only
occurrence of a forbidden construct is inside a comment is accepted, a file with a real
one is rejected, and a file with no statements is rejected for having none.

**`verify` compares parsed forms, not text, with one parser.** `SHOW CREATE TABLE`
output is itself a `CREATE TABLE` statement, so `ddl.parse_create_table` runs over the
file and over the server and the canonical forms are diffed. A second parser for the
server's dialect would be a second implementation of the thing being checked.
`prove-verify` builds a deliberately-wrong table in a throwaway database, asserts the
three mutations reached the server, and asserts the comparison reports all three.
