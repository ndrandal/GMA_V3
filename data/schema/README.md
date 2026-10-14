# `data/schema` — the L1 ClickHouse DDL and its gates

| file | what it is |
|---|---|
| `clickhouse.sql` | **the schema.** Six tables: `trades`, `book_events`, `book_snapshots`, `symbology`, `ingest_windows`, `gaps`. Every statement is `IF NOT EXISTS`. |
| `apply.sh` | the acceptance run: both self-tests, apply from scratch, apply twice more, `SHOW CREATE TABLE` comparison, and a demonstration that the comparison can fail. |
| `apply.py` | `apply` / `verify` / `prove-verify` / `resolve-target` / `self-test` / `emit`. |
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
./apply.sh --show-target              # which server am I about to talk to?
./apply.sh                            # idempotent apply + verify
./apply.sh --fresh                    # DROP DATABASE gma first — DESTRUCTIVE
python3 check_no_compute.py lint      # no server needed
python3 check_no_compute.py self-test # proves the lint can fail
python3 apply.py self-test            # proves the comparison can fail; no server needed
```

Exit codes everywhere: **0** clean, **1** a finding or a difference, **2** could not
run. Those are three colours on purpose — "could not run" and "found a violation"
wearing the same one is the failure this project keeps hitting.

## Which server, and why it is safety-critical

```
--url <u>   >   CH_HTTP_URL   >   CH_HTTP_PORT   >   8123 + STACK_PORT_OFFSET
```

The last term is exactly how `tools/stack.sh` derives `CH_HTTP_PORT`, so a worktree at
`STACK_PORT_OFFSET=270` reaches **its own** server on 8393. The first version of
`apply.py` hardcoded `http://127.0.0.1:8123/`, which meant `./apply.sh --fresh` from
any worktree ran `DROP DATABASE gma` on the **offset-zero** store — somebody else's L1.
`apply.sh --show-target` prints the resolution and touches nothing.

`--fresh` carries two refusals, and the first runs **before any network call** so that
an unreachable endpoint cannot stand in for an opinion about whether you meant it:

1. a non-zero `STACK_PORT_OFFSET` with no explicit target — a destructive default must
   not be reachable by being in the wrong directory;
2. a target that holds rows, unless `--yes-destroy-rows`. A guard that only works while
   the store is empty is not a guard, and the store stops being empty at ENC-1483.

## The three properties worth knowing

**The lint strips SQL comments before it scans, and that is not a convenience.** The
schema's own header documents every construct the lint forbids, by name. Scanning the
raw text reports the documentation as the violation — 9 occurrences, today. Scanning
nothing at all reports a clean pass. `self-test` pins both ends: a comment-only-mentions
fixture is accepted, real violations are rejected, and a file with no statements is
rejected for having none. It also generates one mutant per committed decision and
requires each to redden.

**`verify` compares parsed forms, not text, with one parser.** `SHOW CREATE TABLE`
output is itself a `CREATE TABLE` statement, so `ddl.parse_create_table` runs over the
file and over the server and the canonical forms are diffed — over the **union** of
clause keys in both directions. That matters because `apply` is `IF NOT EXISTS`-only and
can therefore never add a clause to an existing table: a `TTL` the file declares and the
server lacks is the case that bites, and the first implementation reported it as a match.

**`prove-verify` and both `self-test`s exist so no gate here is unobserved.**
`prove-verify` builds a deliberately-wrong table in a throwaway database, asserts the
three mutations reached the server, then asserts the comparison reports all three.
`apply.py self-test` drives `_diff_table`, `_bare_name` and `resolve_target` over cases
that must differ and cases that must not, with no server at all.
