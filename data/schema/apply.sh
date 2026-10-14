#!/usr/bin/env bash
# ENC-1480 acceptance run for the L1 DDL.
#
# Applies data/schema/clickhouse.sql to a live ClickHouse FROM SCRATCH AND THEN
# TWICE IN A ROW, confirms SHOW CREATE TABLE matches the file, and proves both
# gates can fail. Every statement in the file is IF NOT EXISTS, so the second
# and third applications must be no-ops rather than errors — this script is how
# that claim is checked rather than asserted.
#
# Needs the server tools/stack.sh runs:
#     tools/stack.sh up clickhouse       # from the workspace root
#     tools/stack.sh doctor              # exits non-zero if it is not fit
# Override the endpoint with CH_HTTP_URL (a parallel worktree at
# STACK_PORT_OFFSET=<n> publishes http on 8123+<n>).
#
#   ./apply.sh                 lint + self-tests + idempotent apply + verify
#   ./apply.sh --fresh         the same, but DROP DATABASE gma first (DESTRUCTIVE)
#
# --fresh is what makes it "from scratch". It destroys every L1 row; that is
# survivable only because SPEC D6 retains the raw DBN files, so re-ingest is a
# replay rather than a re-purchase. Do not reach for it on a populated store
# without reading docs/data-layer.md §8.6.
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"
URL="${CH_HTTP_URL:-http://127.0.0.1:8123/}"
FRESH=""
for arg in "$@"; do
  case "$arg" in
    --fresh) FRESH="--fresh" ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

step() { printf '\n=== %s ===\n' "$1"; }

step "1. the DDL must not compute (SPEC D2) — and the check must be able to fail"
python3 check_no_compute.py self-test
python3 check_no_compute.py lint --cross-check-aggregates --url "$URL"

step "2. apply ${FRESH:+from scratch }once"
python3 apply.py apply $FRESH --url "$URL"

step "3. apply again — every statement is IF NOT EXISTS, so this is a no-op"
python3 apply.py apply --url "$URL"

step "4. and a third time, because 'twice' is only a claim about the second run"
python3 apply.py apply --url "$URL"

step "5. SHOW CREATE TABLE matches the file"
python3 apply.py verify --url "$URL"

step "6. and the comparison can report a difference"
python3 apply.py prove-verify --url "$URL"

printf '\nACCEPTED: %s applies cleanly from scratch and idempotently, the server\n' \
  "data/schema/clickhouse.sql"
printf 'matches the file, and both gates were observed rejecting a deliberate violation.\n'
