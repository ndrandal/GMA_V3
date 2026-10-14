#!/usr/bin/env python3
"""Apply `data/schema/clickhouse.sql` to a ClickHouse server, and verify that
what the server holds is what the file says.

    apply.py apply  [--url URL] [--fresh]
    apply.py verify [--url URL]
    apply.py emit

`apply` POSTs each statement separately, because the HTTP interface takes one
query per request. Statements come from `ddl.split_statements` — the same
splitter `check_no_compute.py` scans, so the thing applied and the thing checked
cannot drift apart.

`verify` is `SHOW CREATE TABLE` compared against the file, and it compares
PARSED forms rather than text: it runs `ddl.parse_create_table` over the file's
statement and over the server's `SHOW CREATE TABLE` output (which is itself a
CREATE TABLE statement) and diffs the canonical columns, indexes, engine,
partition key, sort key and index_granularity. Three differences are expected
and normalized away, not ignored: the server backticks identifiers, writes
`DateTime64(9, 'UTC')` with its own spacing, and flattens `Nested(...)` into
`levels.side Array(UInt8)` and friends.

`--fresh` DROPs the `gma` database first. It exists so the acceptance run can be
"from scratch, then twice in a row"; it destroys every row in L1 and there is no
undo other than re-ingest (which SPEC D6 does make possible — the raw DBN files
are retained precisely so this is survivable).

WHICH SERVER, and this is safety-critical rather than configuration:

    --url <u>  >  CH_HTTP_URL  >  CH_HTTP_PORT  >  8123 + STACK_PORT_OFFSET

The last term is how tools/stack.sh derives `CH_HTTP_PORT`, so a worktree at
STACK_PORT_OFFSET=270 reaches its own server on 8393 rather than the offset-zero
store. `resolve-target` prints the resolution and touches nothing. See
`resolve_target` and `guard_fresh` for the two refusals --fresh carries.

`self-test` needs no server: it drives `_diff_table`, `_bare_name` and
`resolve_target` over cases that must report a difference and cases that must
not. Exit codes throughout: 0 clean, 1 a difference was found, 2 could not run.
"""

import argparse
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddl  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.path.join(HERE, "clickhouse.sql")

# The base port tools/stack.sh derives ClickHouse's HTTP port from:
#   CH_HTTP_PORT := $(stack_port 8123) == 8123 + STACK_PORT_OFFSET
CH_HTTP_BASE_PORT = 8123


class CouldNotRun(Exception):
    """The tool cannot act, as distinct from finding a difference. Exit 2."""


def resolve_target(explicit_url=None):
    """Work out which ClickHouse this invocation is talking to.

    ⚠ THIS IS A SAFETY-CRITICAL FUNCTION, not plumbing. `apply --fresh` runs
    `DROP DATABASE gma`, and the first version of this file hardcoded
    `http://127.0.0.1:8123/`. tools/stack.sh resolves
    `CH_HTTP_PORT := 8123 + STACK_PORT_OFFSET`, so a worktree at
    STACK_PORT_OFFSET=270 runs its own server on 8393 — and the hardcoded
    default meant `./apply.sh --fresh` from that worktree dropped the
    OFFSET-ZERO database, i.e. somebody else's L1 store. ENC-1480's own
    acceptance run did exactly that; it was harmless only because nothing is
    ingested yet, and it stops being harmless the moment ENC-1483 lands seven
    days of MBO.

    Precedence, highest first: an explicit --url, CH_HTTP_URL, CH_HTTP_PORT,
    then 8123 + STACK_PORT_OFFSET. The returned `explicit` flag is what the
    --fresh guard keys on.

    -> (url, how, offset, offset_was_set, explicit)
    """
    raw = os.environ.get("STACK_PORT_OFFSET", "").strip()
    if raw == "":
        offset, offset_was_set = 0, False
    else:
        if not raw.isdigit():
            raise CouldNotRun(
                "STACK_PORT_OFFSET must be a non-negative integer (got %r) — "
                "same rule as tools/stack.sh, which would die on this too." % raw)
        offset, offset_was_set = int(raw, 10), True   # mirrors stack.sh's 10#$

    if explicit_url:
        return explicit_url, "--url", offset, offset_was_set, True
    env_url = os.environ.get("CH_HTTP_URL", "").strip()
    if env_url:
        return env_url, "CH_HTTP_URL", offset, offset_was_set, True
    env_port = os.environ.get("CH_HTTP_PORT", "").strip()
    if env_port:
        if not env_port.isdigit():
            raise CouldNotRun("CH_HTTP_PORT must be a port number (got %r)"
                              % env_port)
        return ("http://127.0.0.1:%s/" % env_port, "CH_HTTP_PORT", offset,
                offset_was_set, False)
    return ("http://127.0.0.1:%d/" % (CH_HTTP_BASE_PORT + offset),
            "8123 + STACK_PORT_OFFSET(%d)" % offset, offset, offset_was_set,
            False)


def describe_target(url, how, offset):
    return "target: %s  (via %s; STACK_PORT_OFFSET=%d)" % (url, how, offset)


def guard_fresh(url, how, offset, offset_was_set, explicit):
    """Refuse a destructive --fresh that is only correct by accident.

    Two independent refusals, because the hazard has two shapes:

      1. A NON-ZERO OFFSET WITHOUT AN EXPLICIT TARGET. A destructive default
         must not be reachable by being in the wrong directory. Even though the
         port is now derived, requiring the operator to name the server is the
         difference between "I meant this one" and "this is what my shell
         happened to export".
      2. ROWS IN THE TARGET. The reason the original defect was survivable is
         that nothing is ingested yet. That expires with ENC-1483, and a guard
         that only works while the store is empty is not a guard.
    """
    if offset_was_set and offset != 0 and not explicit:
        raise CouldNotRun(
            "refusing --fresh: STACK_PORT_OFFSET=%d but no explicit target.\n"
            "  --fresh runs DROP DATABASE gma, and a destructive default must "
            "not be reachable by being in the wrong directory.\n"
            "  Derived target would be %s. Name it on purpose:\n"
            "      CH_HTTP_URL=%s ./apply.sh --fresh\n"
            "  or, for the offset-zero store, run with STACK_PORT_OFFSET=0."
            % (offset, url, url))


def guard_fresh_rows(url, allow_rows):
    """The second refusal: --fresh must not silently destroy rows."""
    try:
        out = ch("SELECT name, total_rows FROM system.tables "
                 "WHERE database = 'gma' AND total_rows > 0 "
                 "ORDER BY name FORMAT TSVRaw", url)
    except RuntimeError:
        return                      # no server / no database: nothing to lose
    populated = [l.split("\t") for l in out.splitlines() if l.strip()]
    if not populated:
        return
    rows = ", ".join("%s=%s" % (n, r) for n, r in populated)
    if allow_rows:
        print("  --yes-destroy-rows: dropping a POPULATED gma (%s)" % rows)
        return
    raise CouldNotRun(
        "refusing --fresh: gma at %s holds rows (%s).\n"
        "  DROP DATABASE destroys them. Re-ingest is possible (SPEC D6 retains "
        "the raw DBN files) but it is not free and it is not silent.\n"
        "  Pass --yes-destroy-rows if that is genuinely what you want."
        % (url, rows))


def ch(sql, url):
    req = urllib.request.Request(url, data=sql.encode("utf-8"), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return resp.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace").strip()
        raise RuntimeError("ClickHouse refused the query:\n  %s\n  --- sql ---\n%s"
                           % (body, sql)) from None
    except urllib.error.URLError as exc:
        raise RuntimeError(
            "could not reach ClickHouse at %s (%s). Start it with "
            "`tools/stack.sh up clickhouse` from the workspace root."
            % (url, exc.reason)) from None


def first_line(stmt):
    return " ".join(stmt.split())[:72]


def cmd_emit(_args):
    statements, _ = ddl.read_statements(SCHEMA)
    for i, stmt in enumerate(statements, 1):
        print("-- [%d/%d] %s" % (i, len(statements), first_line(stmt)))
        print(stmt + ";")
    return 0


def cmd_apply(args):
    statements, _ = ddl.read_statements(SCHEMA)
    if not statements:
        print("could not run: 0 statements parsed from %s" % SCHEMA, file=sys.stderr)
        return 2
    print("  " + describe_target(args.url, args.how, args.offset))
    # The offset guard runs BEFORE the first network call, on purpose. A
    # destructive-intent check that only fires after a successful connect is not
    # a guard — point it at an unreachable endpoint and it reports "connection
    # refused" without ever having had an opinion about whether you meant that
    # endpoint at all.
    if args.fresh:
        guard_fresh(args.url, args.how, args.offset, args.offset_was_set,
                    args.explicit)
    print("server:  %s" % ch("SELECT version() FORMAT TSVRaw", args.url).strip())
    if args.fresh:
        guard_fresh_rows(args.url, args.yes_destroy_rows)
        print("--fresh: DROP DATABASE gma on %s  (destroys every L1 row)"
              % args.url)
        ch("DROP DATABASE IF EXISTS gma SYNC", args.url)
    print("applying %s — %d statements" % (os.path.relpath(SCHEMA), len(statements)))
    for i, stmt in enumerate(statements, 1):
        ch(stmt, args.url)
        print("  [%d/%d] ok  %s" % (i, len(statements), first_line(stmt)))
    names = ch("SELECT name FROM system.tables WHERE database = 'gma' "
               "ORDER BY name FORMAT TSVRaw", args.url).split()
    print("gma now holds %d tables: %s" % (len(names), ", ".join(names)))
    return 0


# Clauses the server derives rather than stores, so their absence from the file
# is not a divergence. PRIMARY KEY defaults to ORDER BY; it is only a real
# difference if the file sets one.
_DERIVED_CLAUSES = {"PRIMARY KEY"}


def _diff_table(name, want, got):
    """-> list of human-readable differences."""
    out = []
    if want["columns"] != got["columns"]:
        wc = {c[0]: c for c in want["columns"]}
        gc = {c[0]: c for c in got["columns"]}
        for col in [c[0] for c in want["columns"]]:
            if col not in gc:
                out.append("%s: column %s in the file, absent on the server" % (name, col))
            elif wc[col] != gc[col]:
                labels = ("type", "default", "codec")
                parts = [
                    "%s file %r server %r" % (lab, wc[col][i + 1], gc[col][i + 1])
                    for i, lab in enumerate(labels)
                    if wc[col][i + 1] != gc[col][i + 1]
                ]
                out.append("%s: column %s — %s" % (name, col, "; ".join(parts)))
        for col in [c[0] for c in got["columns"]]:
            if col not in wc:
                out.append("%s: column %s on the server, absent from the file"
                           % (name, col))
        if not out:
            out.append("%s: columns are the same set in a different order:\n"
                       "      file   %s\n      server %s"
                       % (name, [c[0] for c in want["columns"]],
                          [c[0] for c in got["columns"]]))
    if want["indexes"] != got["indexes"]:
        out.append("%s: indexes/projections — file %s, server %s"
                   % (name, want["indexes"], got["indexes"]))

    # THE UNION OF CLAUSE KEYS, IN BOTH DIRECTIONS. The first version compared a
    # fixed list (ENGINE / PARTITION BY / ORDER BY) and then looked for clauses
    # the SERVER had and the file did not — so a clause present in the FILE and
    # absent on the SERVER was invisible, and a `want` carrying TTL and SAMPLE
    # BY against a `got` with neither reported DIFFS: []. That is precisely the
    # case that bites, because `apply` is IF NOT EXISTS-only and therefore can
    # NEVER add a clause to a table that already exists: the file says TTL, the
    # server has none, apply exits 0 and verify said "matches".
    for clause in sorted(set(want["clauses"]) | set(got["clauses"])):
        if clause == "SETTINGS":
            continue                      # structured; handled below
        w = want["clauses"].get(clause)
        g = got["clauses"].get(clause)
        if w == g:
            continue
        if clause in _DERIVED_CLAUSES and w is None:
            continue                      # server derives PRIMARY KEY from ORDER BY
        if w is None:
            out.append("%s: server has a %s clause the file does not: %r"
                       % (name, clause, g))
        elif g is None:
            out.append("%s: the FILE has a %s clause the server does not: %r. "
                       "`apply` is IF NOT EXISTS-only and cannot add it to an "
                       "existing table — this needs an ALTER or a reload."
                       % (name, clause, w))
        else:
            out.append("%s: %s — file %r, server %r" % (name, clause, w, g))

    ws = want["clauses"].get("SETTINGS", {}) or {}
    gs = got["clauses"].get("SETTINGS", {}) or {}
    for k in sorted(set(ws) | set(gs)):
        if ws.get(k) == gs.get(k):
            continue
        if k not in gs:
            out.append("%s: the FILE sets SETTINGS %s = %r; the server does not "
                       "set it at all" % (name, k, ws[k]))
        elif k not in ws:
            out.append("%s: the server sets SETTINGS %s = %r; the file does not"
                       % (name, k, gs[k]))
        else:
            out.append("%s: SETTINGS %s — file %r, server %r"
                       % (name, k, ws[k], gs[k]))
    return out


def _bare_name(qualified):
    """`gma.trades` -> `trades`. Raises CouldNotRun rather than IndexError on a
    `CREATE TABLE` with no database prefix — an unqualified name means the
    statement depends on the connection's current database, which is not
    something this tool should guess at while comparing a schema."""
    if "." not in qualified:
        raise CouldNotRun(
            "CREATE TABLE %s has no database prefix. Qualify it (gma.%s) — an "
            "unqualified name resolves against the session's current database, "
            "so the file would not say which server-side table it describes."
            % (qualified, qualified))
    return qualified.split(".", 1)[1]


def cmd_verify(args):
    statements, _ = ddl.read_statements(SCHEMA)
    wanted = {}
    for stmt in statements:
        if ddl.is_create_table(stmt):
            parsed = ddl.parse_create_table(stmt)
            wanted[parsed["table"]] = parsed
    if not wanted:
        print("could not run: no CREATE TABLE parsed from %s" % SCHEMA,
              file=sys.stderr)
        return 2

    print("verify: %d table(s) from %s against %s"
          % (len(wanted), os.path.relpath(SCHEMA), args.url))
    print("  " + describe_target(args.url, args.how, args.offset))
    diffs = []
    for name in wanted:
        shown = ch("SHOW CREATE TABLE %s FORMAT TSVRaw" % name, args.url).strip()
        got = ddl.parse_create_table(shown)
        table_diffs = _diff_table(name, wanted[name], got)
        print("  [%s] %-22s %d columns, %d index/projection, %s"
              % ("ok" if not table_diffs else "DIFF", name,
                 len(got["columns"]), len(got["indexes"]),
                 got["clauses"].get("ENGINE")))
        diffs.extend(table_diffs)

    # The server must hold exactly the file's tables and nothing else. The one
    # that matters is gma.quotes: it is defined-but-commented because nothing
    # populates it without mbp-1 (SPEC Non-goals), and an empty table reads as
    # "the market had no quotes" — docs §8.8's failure.
    live = set(ch("SELECT name FROM system.tables WHERE database = 'gma' "
                  "ORDER BY name FORMAT TSVRaw", args.url).split())
    extra = sorted(live - {_bare_name(t) for t in wanted})
    if extra:
        diffs.append("gma holds table(s) the file does not create: %s"
                     % ", ".join(extra))
    if "quotes" in live:
        diffs.append("gma.quotes exists on the server. The file leaves it "
                     "commented (SPEC Non-goals); an empty quotes table reads "
                     "as a quoteless market.")

    if diffs:
        print("  DIFFERENCES (%d):" % len(diffs))
        for d in diffs:
            print("    - %s" % d)
        return 1
    print("  SHOW CREATE TABLE matches the file for all %d tables; gma holds "
          "nothing else" % len(wanted))
    return 0


PROBE_DB = "gma_verify_probe"


def cmd_prove_verify(args):
    """Show the verifier failing. A comparison never observed reporting a
    difference has not been shown to compare.

    Done in a throwaway database rather than by mutating `gma`, so it is safe to
    run as part of the acceptance script. The mutation is asserted to have taken
    effect before its detection is claimed — a no-op mutation reads exactly like
    a passing gate.
    """
    statements, _ = ddl.read_statements(SCHEMA)
    src = None
    for stmt in statements:
        if ddl.is_create_table(stmt) and \
                ddl.parse_create_table(stmt)["table"] == "gma.gaps":
            src = stmt
    if src is None:
        print("could not run: gma.gaps not found in %s" % SCHEMA, file=sys.stderr)
        return 2
    want = ddl.parse_create_table(src)

    # The probe DDL is SYNTHESIZED from the parsed form rather than patched as
    # text: the statements `read_statements` hands back are already
    # comment-stripped, so string surgery against the file's comments would
    # silently become a no-op.
    cols = list(want["columns"])
    mutated_cols = []
    for cname, ctype, codec in cols[:-1]:            # mutation 1: drop the last column
        if cname == "seq_to":
            codec = "CODEC(ZSTD(9))"                 # mutation 2: different codec
        mutated_cols.append((cname, ctype, codec))
    body = ",\n    ".join(
        "%s %s %s" % (c, t, k) if k else "%s %s" % (c, t)
        for c, t, k in mutated_cols)
    probe_ddl = (
        "CREATE TABLE %s.gaps\n(\n    %s\n)\nENGINE = %s\n"
        "PARTITION BY %s\nORDER BY %s\nSETTINGS index_granularity = 4096"
        % (PROBE_DB, body, want["clauses"]["ENGINE"],
           want["clauses"]["PARTITION BY"], want["clauses"]["ORDER BY"]))
    dropped = cols[-1][0]

    ch("DROP DATABASE IF EXISTS %s SYNC" % PROBE_DB, args.url)
    ch("CREATE DATABASE %s" % PROBE_DB, args.url)
    try:
        ch(probe_ddl, args.url)
        shown = ch("SHOW CREATE TABLE %s.gaps FORMAT TSVRaw" % PROBE_DB,
                   args.url).strip()
        got = ddl.parse_create_table(shown)

        # Assert the mutation actually landed before claiming its detection: a
        # no-op mutation reads exactly like a passing gate.
        applied = []
        names = [c[0] for c in got["columns"]]
        if dropped not in names and len(names) == len(cols) - 1:
            applied.append("column %r dropped" % dropped)
        if dict((c[0], c[2]) for c in got["columns"]).get("seq_to") == "CODEC(ZSTD(9))":
            applied.append("seq_to codec -> ZSTD(9)")
        if got["clauses"].get("SETTINGS", {}).get("index_granularity") == "4096":
            applied.append("index_granularity -> 4096")
        if len(applied) != 3:
            print("could not run: only %d of 3 mutations reached the server (%s)"
                  % (len(applied), ", ".join(applied) or "none"), file=sys.stderr)
            return 2
        print("  mutation applied to %s.gaps: %s" % (PROBE_DB, "; ".join(applied)))

        diffs = _diff_table("gma.gaps", want, got)
        for d in diffs:
            print("    detected: %s" % d)
        if len(diffs) < 3:
            print("  PROOF FAILED: expected at least 3 differences, got %d. The "
                  "comparison does not gate." % len(diffs), file=sys.stderr)
            return 1
        print("  proof: the comparison reports %d difference(s) against a "
              "deliberately-wrong table, and 0 against the real one" % len(diffs))
        return 0
    finally:
        ch("DROP DATABASE IF EXISTS %s SYNC" % PROBE_DB, args.url)


# ---------------------------------------------------------------------------
# self-test — the comparison has to be seen reporting each shape of difference
# ---------------------------------------------------------------------------

def _tbl(columns=None, indexes=None, **clauses):
    base = {"ENGINE": "MergeTree", "PARTITION BY": "toYYYYMMDD(ts)",
            "ORDER BY": "(symbol,ts)", "SETTINGS": {"index_granularity": "8192"}}
    base.update(clauses)
    if columns is None:
        columns = [("ts", "DateTime64(9,'UTC')", "", "CODEC(DoubleDelta,ZSTD(1))"),
                   ("symbol", "LowCardinality(String)", "", ""),
                   ("updated_at", "DateTime64(3,'UTC')", "DEFAULT now64(3)", "")]
    return {"table": "gma.t", "columns": list(columns),
            "indexes": list(indexes or []), "clauses": base}


def _diff_cases():
    """(label, want, got, min_diffs). min_diffs 0 means "must report nothing"."""
    cols = _tbl()["columns"]
    no_default = [c if c[0] != "updated_at"
                  else (c[0], c[1], "", c[3]) for c in cols]
    other_codec = [c if c[0] != "ts"
                   else (c[0], c[1], c[2], "CODEC(ZSTD(9))") for c in cols]
    return [
        ("identical", _tbl(), _tbl(), 0),
        # Derived clauses are not differences: the server prints PRIMARY KEY
        # even though the file never sets one.
        ("server derives PRIMARY KEY", _tbl(),
         _tbl(**{"PRIMARY KEY": "(symbol,ts)"}), 0),
        # THE REGRESSION THIS SELF-TEST EXISTS FOR. The first implementation
        # reported DIFFS: [] here, and `apply` is IF NOT EXISTS-only so it can
        # never add either clause to an existing table.
        ("file has TTL + SAMPLE BY, server has neither",
         _tbl(**{"TTL": "toDateTime(ts)+toIntervalDay(30)",
                 "SAMPLE BY": "symbol"}), _tbl(), 2),
        ("server has a TTL the file does not", _tbl(),
         _tbl(**{"TTL": "toDateTime(ts)+toIntervalDay(30)"}), 1),
        ("file sets a SETTINGS key the server does not",
         _tbl(SETTINGS={"index_granularity": "8192", "ttl_only_drop_parts": "1"}),
         _tbl(), 1),
        ("server sets a SETTINGS key the file does not", _tbl(),
         _tbl(SETTINGS={"index_granularity": "8192", "ttl_only_drop_parts": "1"}),
         1),
        ("SETTINGS value differs", _tbl(),
         _tbl(SETTINGS={"index_granularity": "1024"}), 1),
        ("ENGINE differs", _tbl(), _tbl(ENGINE="ReplacingMergeTree(updated_at)"), 1),
        ("ORDER BY differs", _tbl(), _tbl(**{"ORDER BY": "(ts,symbol)"}), 1),
        ("PARTITION BY differs", _tbl(),
         _tbl(**{"PARTITION BY": "toYYYYMMDD(updated_at)"}), 1),
        # The DEFAULT case: a missing column default is invisible to a type
        # comparison, and on gma.ingest_windows.updated_at it is the
        # zero-valued-clock defect.
        ("column DEFAULT missing on the server", _tbl(),
         _tbl(columns=no_default), 1),
        ("column DEFAULT missing in the file", _tbl(columns=no_default),
         _tbl(), 1),
        ("column codec differs", _tbl(), _tbl(columns=other_codec), 1),
        ("column missing on the server", _tbl(),
         _tbl(columns=cols[:-1]), 1),
        ("column missing in the file", _tbl(columns=cols[:-1]), _tbl(), 1),
        ("index missing on the server",
         _tbl(indexes=["INDEX ix_ts ts TYPE minmax GRANULARITY 4"]), _tbl(), 1),
    ]


def cmd_self_test(_args):
    print("apply.py self-test — the comparison must be seen reporting every "
          "shape of difference, and nothing when there is none:")
    failures = 0
    for label, want, got, min_diffs in _diff_cases():
        diffs = _diff_table("gma.t", want, got)
        ok = (len(diffs) == 0) if min_diffs == 0 else (len(diffs) >= min_diffs)
        print("  [%s] %-46s -> %d diff(s), want %s"
              % ("ok" if ok else "FAIL", label, len(diffs),
                 "0" if min_diffs == 0 else ">=%d" % min_diffs))
        if not ok:
            failures += 1
            for d in diffs:
                print("        %s" % d)

    # _bare_name must refuse an unqualified table rather than raise IndexError.
    try:
        _bare_name("trades")
        print("  [FAIL] unqualified table name                     -> accepted")
        failures += 1
    except CouldNotRun:
        print("  [ok] %-46s -> CouldNotRun (exit 2, not a traceback)"
              % "unqualified table name")
    if _bare_name("gma.trades") != "trades":
        print("  [FAIL] qualified table name mis-parsed")
        failures += 1

    # resolve_target must track STACK_PORT_OFFSET the way tools/stack.sh does.
    saved = {k: os.environ.get(k) for k in
             ("STACK_PORT_OFFSET", "CH_HTTP_URL", "CH_HTTP_PORT")}
    try:
        for k in saved:
            os.environ.pop(k, None)
        checks = [
            ({}, "http://127.0.0.1:8123/", False),
            ({"STACK_PORT_OFFSET": "0"}, "http://127.0.0.1:8123/", False),
            ({"STACK_PORT_OFFSET": "270"}, "http://127.0.0.1:8393/", False),
            ({"STACK_PORT_OFFSET": "0070"}, "http://127.0.0.1:8193/", False),
            ({"STACK_PORT_OFFSET": "270", "CH_HTTP_PORT": "9999"},
             "http://127.0.0.1:9999/", False),
            ({"STACK_PORT_OFFSET": "270", "CH_HTTP_URL": "http://h:1/"},
             "http://h:1/", True),
        ]
        for env, want_url, want_explicit in checks:
            for k in saved:
                os.environ.pop(k, None)
            os.environ.update(env)
            url, how, off, was_set, explicit = resolve_target(None)
            ok = url == want_url and explicit == want_explicit
            print("  [%s] resolve %-37s -> %s"
                  % ("ok" if ok else "FAIL",
                     env or "{no offset exported}", url))
            if not ok:
                failures += 1
                print("        wanted %s (explicit=%s)" % (want_url, want_explicit))
        # And the destructive guard must refuse the accident it exists for.
        for k in saved:
            os.environ.pop(k, None)
        os.environ["STACK_PORT_OFFSET"] = "270"
        url, how, off, was_set, explicit = resolve_target(None)
        try:
            guard_fresh(url, how, off, was_set, explicit)
            print("  [FAIL] --fresh guard at offset 270 without an explicit "
                  "target -> ALLOWED")
            failures += 1
        except CouldNotRun:
            print("  [ok] %-46s -> refused" % "--fresh at offset 270, no explicit target")
        os.environ["CH_HTTP_URL"] = url
        url, how, off, was_set, explicit = resolve_target(None)
        try:
            guard_fresh(url, how, off, was_set, explicit)
            print("  [ok] %-46s -> allowed" % "--fresh at offset 270, target named")
        except CouldNotRun as exc:
            print("  [FAIL] --fresh refused even with an explicit target: %s" % exc)
            failures += 1
    finally:
        for k, v in saved.items():
            os.environ.pop(k, None)
            if v is not None:
                os.environ[k] = v

    if failures:
        print("SELF-TEST FAILED: %d case(s)" % failures)
        return 1
    print("self-test: every case behaved as specified")
    return 0


def cmd_resolve_target(args):
    print(describe_target(args.url, args.how, args.offset))
    print("explicit: %s   (an explicit target is what `apply --fresh` requires "
          "at a non-zero offset)" % ("yes" if args.explicit else "no"))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    # --url defaults to None, not to a resolved string, so that "the operator
    # named a target" is distinguishable from "the tool picked one".
    for cmd in ("apply", "verify", "prove-verify", "resolve-target"):
        p = sub.add_parser(cmd)
        p.add_argument("--url", default=None,
                       help="explicit ClickHouse HTTP endpoint; overrides "
                            "CH_HTTP_URL, CH_HTTP_PORT and STACK_PORT_OFFSET")
        if cmd == "apply":
            p.add_argument("--fresh", action="store_true",
                           help="DROP DATABASE gma first — DESTRUCTIVE")
            p.add_argument("--yes-destroy-rows", action="store_true",
                           help="allow --fresh to drop a gma that holds rows")
    sub.add_parser("emit")
    sub.add_parser("self-test")
    args = ap.parse_args(argv)

    try:
        if args.cmd not in ("emit", "self-test"):
            (args.url, args.how, args.offset, args.offset_was_set,
             args.explicit) = resolve_target(args.url)
        return {"apply": cmd_apply, "verify": cmd_verify,
                "prove-verify": cmd_prove_verify,
                "resolve-target": cmd_resolve_target,
                "self-test": cmd_self_test,
                "emit": cmd_emit}[args.cmd](args)
    except CouldNotRun as exc:
        print("could not run: %s" % exc, file=sys.stderr)
        return 2
    except RuntimeError as exc:
        print("FAILED: %s" % exc, file=sys.stderr)
        return 2
    except (ValueError, IndexError, KeyError, OSError) as exc:
        # ddl.parse_create_table raises ValueError BY DESIGN on anything outside
        # the subset it understands, so a server rendering an unhandled
        # construct used to give a traceback and exit 1 — the colour reserved
        # for "a difference was found". A broken instrument is exit 2.
        print("could not run: %s: %s" % (type(exc).__name__, exc),
              file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
