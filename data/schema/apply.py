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
DEFAULT_URL = os.environ.get("CH_HTTP_URL", "http://127.0.0.1:8123/")


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
    print("server:  %s" % ch("SELECT version() FORMAT TSVRaw", args.url).strip())
    if args.fresh:
        print("--fresh: DROP DATABASE gma  (destroys every L1 row)")
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
                out.append("%s: column %s — file %r, server %r"
                           % (name, col, wc[col][1:], gc[col][1:]))
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
    for clause in ("ENGINE", "PARTITION BY", "ORDER BY"):
        w, g = want["clauses"].get(clause), got["clauses"].get(clause)
        if w != g:
            out.append("%s: %s — file %r, server %r" % (name, clause, w, g))
    ws = want["clauses"].get("SETTINGS", {}) or {}
    gs = got["clauses"].get("SETTINGS", {}) or {}
    for k, v in ws.items():
        if gs.get(k) != v:
            out.append("%s: SETTINGS %s — file %r, server %r" % (name, k, v, gs.get(k)))
    for clause in got["clauses"]:
        if clause in ("SETTINGS", "ENGINE", "PARTITION BY", "ORDER BY"):
            continue
        if clause in _DERIVED_CLAUSES and clause not in want["clauses"]:
            continue
        if clause not in want["clauses"]:
            out.append("%s: server has a %s clause the file does not: %r"
                       % (name, clause, got["clauses"][clause]))
    return out


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
    extra = sorted(live - {t.split(".", 1)[1] for t in wanted})
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


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("apply")
    p.add_argument("--url", default=DEFAULT_URL)
    p.add_argument("--fresh", action="store_true",
                   help="DROP DATABASE gma first — DESTRUCTIVE")
    p = sub.add_parser("verify")
    p.add_argument("--url", default=DEFAULT_URL)
    sub.add_parser("emit")
    args = ap.parse_args(argv)
    try:
        return {"apply": cmd_apply, "verify": cmd_verify,
                "emit": cmd_emit}[args.cmd](args)
    except RuntimeError as exc:
        print("FAILED: %s" % exc, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
