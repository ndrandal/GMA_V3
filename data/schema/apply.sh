#!/usr/bin/env bash
# ENC-1480 acceptance run for the L1 DDL.
#
# Applies data/schema/clickhouse.sql to a live ClickHouse FROM SCRATCH AND THEN
# TWICE IN A ROW, confirms SHOW CREATE TABLE matches the file, and proves every
# gate here can fail. Every statement in the file is IF NOT EXISTS, so the
# second and third applications must be no-ops rather than errors — this script
# is how that claim is checked rather than asserted.
#
# WHICH SERVER IT TALKS TO, because --fresh runs DROP DATABASE gma:
#   --url <u>  >  CH_HTTP_URL  >  CH_HTTP_PORT  >  8123 + STACK_PORT_OFFSET
# the last of which is exactly how tools/stack.sh derives CH_HTTP_PORT, so a
# worktree at STACK_PORT_OFFSET=270 reaches its OWN server on 8393 and not the
# offset-zero store. `./apply.sh --show-target` prints the resolved endpoint
# without touching it. At a non-zero offset, --fresh REFUSES unless the target
# is named explicitly, and it refuses regardless if the target holds rows.
#
# Needs the server tools/stack.sh runs:
#     tools/stack.sh up clickhouse       # from the workspace root
#     tools/stack.sh doctor              # exits non-zero if it is not fit
#
#   ./apply.sh                   lint + self-tests + idempotent apply + verify
#   ./apply.sh --show-target     print the resolved endpoint and stop
#   ./apply.sh --fresh           the same, but DROP DATABASE gma first (DESTRUCTIVE)
#
# --fresh is what makes it "from scratch". It destroys every L1 row; that is
# survivable only because SPEC D6 retains the raw DBN files, so re-ingest is a
# replay rather than a re-purchase. Do not reach for it on a populated store
# without reading docs/data-layer.md §8.6.
set -euo pipefail

SELF="$(readlink -f "$0")"      # resolve before the cd, or --help cannot read it
cd "$(dirname "$SELF")"
FRESH=""
PASSTHRU=()
for arg in "$@"; do
  case "$arg" in
    --fresh) FRESH="--fresh" ;;
    --yes-destroy-rows) PASSTHRU+=("$arg") ;;
    --show-target) exec python3 apply.py resolve-target ;;
    -h|--help) sed -n '2,29p' "$SELF"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

step() { printf '\n=== %s ===\n' "$1"; }

step "0. which server"
python3 apply.py resolve-target

step "1. the DDL must not compute (SPEC D2) — and the check must be able to fail"
python3 check_no_compute.py self-test
python3 check_no_compute.py lint --cross-check-aggregates

step "2. the file/server comparison must be able to report a difference (no server needed)"
python3 apply.py self-test

step "3. apply ${FRESH:+from scratch }once"
python3 apply.py apply $FRESH ${PASSTHRU[@]+"${PASSTHRU[@]}"}

step "4. apply again — every statement is IF NOT EXISTS, so this is a no-op"
python3 apply.py apply

step "5. and a third time, because 'twice' is only a claim about the second run"
python3 apply.py apply

step "6. SHOW CREATE TABLE matches the file"
python3 apply.py verify

step "7. and the comparison can report a difference against a real server"
python3 apply.py prove-verify

printf '\nACCEPTED: %s applies cleanly from scratch and idempotently, the server\n' \
  "data/schema/clickhouse.sql"
printf 'matches the file, and every gate was observed rejecting a deliberate violation.\n'
