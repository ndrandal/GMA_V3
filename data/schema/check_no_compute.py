#!/usr/bin/env python3
"""THE DDL MUST NOT COMPUTE — the committed check for SPEC D2, applied to
`data/schema/clickhouse.sql`.

SPEC `specs/2026-10-14-gma-data-layer/SPEC.md` D1/D2: ClickHouse does selection,
gma does computation, and nothing computes a value twice. D2 is usually read as a
rule about the shim's SELECTs (`gma_shim_sql_lint`, docs §5.4 — a different
check, not this one), but it binds the schema just as hard: a `MATERIALIZED`
column, an `AggregatingMergeTree` or a bar-producing `MATERIALIZED VIEW` is a
second implementation of gma's math that no SELECT lint would ever see, because
by the time a query runs the value has already been computed.

So this asserts a property of the FILE:

  * no aggregate function call, from a 208-name vocabulary read off the server
    (`clickhouse_aggregates.txt`) rather than recalled;
  * no `GROUP BY`;
  * no `MATERIALIZED` or `ALIAS` column, and no view of any kind;
  * no `AggregatingMergeTree` / `SummingMergeTree`;
  * no window function (`OVER (`).

TWO THINGS MAKE IT A GATE RATHER THAN A GESTURE.

1. COMMENTS ARE STRIPPED BEFORE SCANNING. The schema's own header documents
   every one of those prohibitions by name — "no AggregatingMergeTree, no
   SummingMergeTree", "no MATERIALIZED or ALIAS column computing a value" — so a
   naive grep reports the documentation as the violation. `ddl.strip_comments`
   removes `--` to end-of-line and `/* */` while preserving string literals, and
   `self-test` proves both directions: a file whose ONLY occurrence of a
   forbidden construct is inside a comment passes, and a file with a real one
   fails.

2. IT IS ANTI-VACUOUS. A scan that matched nothing because its parser quietly
   produced no input reads exactly like a clean pass (docs §5.4 calls this
   anti-vacuity for the SQL lint; this repo has shipped the failure more than
   once). So the check also asserts a non-zero statement count, that the
   stripper actually removed something, and — for the real schema — that the six
   expected tables are present, that `gma.quotes` is NOT created, that no
   projection is live and that `book_events` carries the plain sort key. Those
   last four are this ticket's decisions, not D2, and they are reported
   separately.

Usage:
  check_no_compute.py lint [FILE] [--no-schema-decisions] [--cross-check-aggregates]
  check_no_compute.py self-test
  check_no_compute.py refresh-aggregates [--url URL]

Exit 0 clean, 1 on a finding, 2 if it could not run.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ddl  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMA = os.path.join(HERE, "clickhouse.sql")
AGGREGATES = os.path.join(HERE, "clickhouse_aggregates.txt")
FIXTURES = os.path.join(HERE, "fixtures")
DEFAULT_URL = os.environ.get("CH_HTTP_URL", "http://127.0.0.1:8123/")

EXPECTED_TABLES = [
    "gma.trades", "gma.book_events", "gma.book_snapshots",
    "gma.symbology", "gma.ingest_windows", "gma.gaps",
]

# Aggregate-function combinators. `sumIf`, `quantilesState`, `anyLastOrNull` are
# the same function with a suffix, so the vocabulary has to tolerate them or the
# cheapest possible evasion walks straight through.
COMBINATORS = (
    "If|Array|ArrayIf|Map|MapIf|SimpleState|State|Merge|MergeState|"
    "OrNull|OrDefault|Resample|Distinct|ForEach|Null|ArgMin|ArgMax"
)


class CouldNotRun(Exception):
    """The instrument is broken, as distinct from the file being wrong.

    Exit 2, never 1. "Could not run" and "found a violation" being the same
    colour is the failure mode this whole project keeps hitting — a check that
    cannot read its own vocabulary must not be reportable as a D2 finding, and
    `raise SystemExit(msg)` exits 1, which is the colour reserved for findings.
    """


def load_aggregates(path=AGGREGATES):
    names = []
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if line and not line.startswith("#"):
                    names.append(line)
    except OSError as exc:
        raise CouldNotRun("cannot read the aggregate vocabulary %s: %s"
                          % (path, exc)) from None
    if not names:
        raise CouldNotRun(
            "%s has no function names, so the aggregate scan would match "
            "nothing and report a clean file. Refusing to pass. Run "
            "`check_no_compute.py refresh-aggregates`." % path)
    return names


def aggregate_regex(names):
    # Longest first so `sumCount` is not shadowed by `sum`.
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    return re.compile(
        r"\b(?:" + alt + r")(?:" + COMBINATORS + r")*\s*\(",
        re.IGNORECASE,
    )


# Structural prohibitions. Each is (label, compiled regex, why).
STRUCTURAL = [
    ("GROUP BY", re.compile(r"\bGROUP\s+BY\b", re.I),
     "a GROUP BY in the DDL computes; D2 permits selection only"),
    ("MATERIALIZED", re.compile(r"\bMATERIALIZED\b", re.I),
     "a MATERIALIZED column or view computes a value at insert time, where no "
     "SELECT lint can see it"),
    ("ALIAS", re.compile(r"\bALIAS\b", re.I),
     "an ALIAS column computes a value at read time and is invisible in the "
     "projection list"),
    ("AggregatingMergeTree", re.compile(r"\bAggregatingMergeTree\b", re.I),
     "an aggregating engine computes during merges — SPEC D2/D4"),
    ("SummingMergeTree", re.compile(r"\bSummingMergeTree\b", re.I),
     "a summing engine computes during merges — SPEC D2/D4"),
    ("window function", re.compile(r"\bOVER\s*\(", re.I),
     "a window function is computation"),
    ("CREATE VIEW", re.compile(r"\bCREATE\s+(?:OR\s+REPLACE\s+)?"
                               r"(?:MATERIALIZED\s+|LIVE\s+|WINDOW\s+)?VIEW\b", re.I),
     "any view puts an expression in a projection list the planner does not "
     "control"),
]


def _line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def scan(path, schema_decisions=True, aggregates_path=AGGREGATES):
    """-> (findings, facts). findings empty == pass."""
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()
    stripped = ddl.strip_comments(raw)
    statements = ddl.split_statements(stripped)

    findings, facts = [], {}
    facts["file"] = path
    facts["raw_bytes"] = len(raw)
    facts["scanned_bytes"] = len(stripped.strip())
    facts["comment_bytes_removed"] = len(raw) - len(stripped)
    facts["statements"] = len(statements)

    # --- anti-vacuity, before any scanning verdict is worth anything --------
    if not statements:
        findings.append("ANTI-VACUITY: 0 statements parsed out of %s. A scan "
                        "with no input cannot pass." % path)
        return findings, facts
    if facts["scanned_bytes"] == 0:
        findings.append("ANTI-VACUITY: the comment-stripped text is empty.")
        return findings, facts

    # --- D2: the DDL must not compute --------------------------------------
    agg_re = aggregate_regex(load_aggregates(aggregates_path))
    for m in agg_re.finditer(stripped):
        findings.append("D2 line %d: aggregate function call %r — the DDL must "
                        "not compute (SPEC D2)"
                        % (_line_of(stripped, m.start()), m.group(0)))
    for label, rx, why in STRUCTURAL:
        for m in rx.finditer(stripped):
            findings.append("D2 line %d: %s — %s"
                            % (_line_of(stripped, m.start()), label, why))

    # --- ENC-1480's schema decisions ---------------------------------------
    if schema_decisions:
        tables = {}
        for stmt in statements:
            if ddl.is_create_table(stmt):
                parsed = ddl.parse_create_table(stmt)
                tables[parsed["table"]] = parsed
        facts["tables"] = sorted(tables)

        for want in EXPECTED_TABLES:
            if want not in tables:
                findings.append("ENC-1480: %s is not created by this file" % want)
        if "gma.quotes" in tables:
            findings.append(
                "ENC-1480: gma.quotes is created. Nothing populates it without "
                "mbp-1 (SPEC Non-goals) and an empty table reads as 'the market "
                "had no quotes' — the §8.8 failure. It stays commented.")
        if re.search(r"\bADD\s+PROJECTION\b", stripped, re.I):
            findings.append(
                "ENC-1480: a projection is live. SPEC D4 defers them — at one "
                "symbol (ts_event, symbol) and (symbol, ts_event, …) are the "
                "same order, so it buys nothing and costs write amplification "
                "on the table B1/B2 measure.")
        # SPEC D5: plain MergeTree on every event table. Its dedup is exact
        # only on a unique sort key, and book_events is not unique (one DBN
        # message yields a book mutation AND a print), so a background merge
        # could silently delete a real row in a way indistinguishable from a
        # provider gap.
        for name in ("gma.trades", "gma.book_events", "gma.book_snapshots",
                     "gma.gaps"):
            t = tables.get(name)
            if t is not None and t["clauses"].get("ENGINE") != "MergeTree":
                findings.append(
                    "ENC-1480: %s uses ENGINE %s. SPEC D5 requires plain "
                    "MergeTree on the event tables." % (name,
                                                        t["clauses"].get("ENGINE")))

        # gma.symbology is the ONE exception, and its version column must stay a
        # WRITE clock. ingest_windows was the second exception and is not any
        # more: ReplacingMergeTree(completed_at) resolves a re-ingest to the
        # stale complete row and deletes the in-flight one at merge time, which
        # makes an incomplete window read as complete — measured under
        # ENC-1480, see the table's comment. Re-adding it reintroduces exactly
        # the D11 failure.
        sym = tables.get("gma.symbology")
        if sym is not None and \
                sym["clauses"].get("ENGINE") != "ReplacingMergeTree(ingested_at)":
            findings.append(
                "ENC-1480: gma.symbology ENGINE is %s, expected "
                "ReplacingMergeTree(ingested_at) — the version column has to be "
                "a write clock or closing a half-open interval resolves "
                "arbitrarily (SPEC D7)." % sym["clauses"].get("ENGINE"))
        iw = tables.get("gma.ingest_windows")
        if iw is not None:
            if iw["clauses"].get("ENGINE") != "MergeTree":
                findings.append(
                    "ENC-1480: gma.ingest_windows ENGINE is %s. It is a plain "
                    "MergeTree append-only transition log: a Replacing engine "
                    "resolves a re-ingest to the stale state=1 row and then "
                    "deletes the in-flight one, so an incomplete window reads "
                    "as complete (SPEC D11, measured)."
                    % iw["clauses"].get("ENGINE"))
            # The exact key, not a suffix test. to_ts must be IN it, or a
            # recorded [00:00,12:00) answers "complete" for a request covering
            # [00:00,24:00) and returns half a day as if whole. And session_id
            # must NOT sit between the window prefix and the clock, or the tail
            # of the prefix range is the newest row of the highest session_id
            # rather than the newest row — and nothing makes session_id
            # time-monotonic.
            want_iw = "(symbol,kind,from_ts,to_ts,updated_at)"
            got_iw = iw["clauses"].get("ORDER BY")
            if got_iw != want_iw:
                findings.append(
                    "ENC-1480: gma.ingest_windows ORDER BY is %s, expected %s. "
                    "to_ts must be in the key (a recorded [00:00,12:00) must "
                    "not answer 'complete' for [00:00,24:00) — D11), and "
                    "session_id must not sit between the window prefix and "
                    "updated_at (it is not time-monotonic, so the prefix tail "
                    "would be the newest row of the highest session, not the "
                    "newest row)." % (got_iw, want_iw))
            # The write clock needs a DEFAULT or "set on every row" is a
            # convention with nothing behind it: measured, an INSERT omitting
            # the column stores epoch 0, every row ties, and "the single newest
            # row" becomes arbitrary — the same zero-valued-clock defect that
            # disqualified ReplacingMergeTree(completed_at).
            iw_cols = {c[0]: c for c in iw["columns"]}
            ua = iw_cols.get("updated_at")
            if ua is None:
                findings.append("ENC-1480: gma.ingest_windows has no "
                                "updated_at column to order its log by.")
            elif "DEFAULT now64(3)" not in ua[2]:
                findings.append(
                    "ENC-1480: gma.ingest_windows.updated_at has no "
                    "`DEFAULT now64(3)` (expression is %r). Without it an "
                    "INSERT that omits the column stores epoch 0 — measured — "
                    "every row ties and 'the newest row' is arbitrary, so a "
                    "stale state=1 can win (SPEC D11)." % (ua[2] or "none"))
            if "state" in iw_cols and "3 invalidated" not in raw:
                findings.append(
                    "ENC-1480: gma.ingest_windows.state no longer documents "
                    "state 3 (invalidated). A DROP PARTITION for one symbol "
                    "wipes every symbol's rows for that day, and 'collaterally "
                    "wiped' is neither in-flight nor failed.")

        # gaps must be partitioned on the day of the HOLE, so the §8.6
        # DROP PARTITION reload can reach it. On ts_detected it cannot: a
        # corrected re-pull leaves a row still asserting a discontinuity in data
        # that is now complete — a phantom hole.
        gp = tables.get("gma.gaps")
        if gp is not None:
            if gp["clauses"].get("PARTITION BY") != "toYYYYMMDD(ts_from)":
                findings.append(
                    "ENC-1480: gma.gaps PARTITION BY is %s, expected "
                    "toYYYYMMDD(ts_from). On the detection day the re-ingest "
                    "DROP PARTITION cannot remove these rows, so a filled hole "
                    "leaves a phantom one behind."
                    % gp["clauses"].get("PARTITION BY"))
            if gp["clauses"].get("ORDER BY") != "(symbol,ts_from,seq_from)":
                findings.append(
                    "ENC-1480: gma.gaps ORDER BY is %s, expected "
                    "(symbol,ts_from,seq_from) — it must lead with the same "
                    "event time the partition key is built on."
                    % gp["clauses"].get("ORDER BY"))
        replacing = sorted(n for n, t in tables.items()
                           if "Replacing" in (t["clauses"].get("ENGINE") or ""))
        if replacing != ["gma.symbology"]:
            findings.append(
                "ENC-1480: the Replacing-engine tables are %s; exactly one "
                "(gma.symbology) is permitted — SPEC D5 is plain MergeTree and "
                "each exception has to earn itself in the file." % replacing)

        be = tables.get("gma.book_events")
        if be is not None:
            got = be["clauses"].get("ORDER BY", "")
            want = "(symbol,ts_event,provider_seq)"
            if got != want:
                findings.append(
                    "ENC-1480: book_events ORDER BY is %s, expected %s. "
                    "Adding `action, order_id` pre-decides the granule-skipping "
                    "question ENC-1484 (T6) exists to measure against real MBO "
                    "(SPEC D5)." % (got, want))

    # How much the comment stripping is actually doing, reported on every run.
    # If this number is 0 the trap is gone and someone has rewritten the
    # schema's header; if the scan were run on the raw text instead, every one
    # of these would be reported as a violation of a rule it is documenting.
    raw_hits = len(agg_re.findall(raw))
    for _label, rx, _why in STRUCTURAL:
        raw_hits += len(rx.findall(raw))
    facts["raw_hits"] = raw_hits
    facts["comment_only_hits"] = raw_hits - len(
        [f for f in findings if f.startswith("D2 ")])

    return findings, facts


# --------------------------------------------------------------------------
# Server access (only for refresh / cross-check — never for the verdict)
# --------------------------------------------------------------------------


def ch_query(sql, url=DEFAULT_URL):
    import urllib.request
    req = urllib.request.Request(url, data=sql.encode("utf-8"), method="POST")
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read().decode("utf-8")


def server_aggregates(url=DEFAULT_URL):
    out = ch_query(
        "SELECT name FROM system.functions WHERE is_aggregate ORDER BY name "
        "FORMAT TSVRaw", url)
    return [l.strip() for l in out.splitlines() if l.strip()]


# --------------------------------------------------------------------------
# self-test — the gate has to be seen failing
# --------------------------------------------------------------------------

SELF_TESTS = [
    # (fixture, must_fail, must_mention, schema_decisions)
    ("clickhouse.sql", False, None, True),
    ("fixtures/comment_only_mentions.sql", False, None, False),
    ("fixtures/violation_aggregating_engine.sql", True, "AggregatingMergeTree", False),
    ("fixtures/violation_materialized_column.sql", True, "MATERIALIZED", False),
    ("fixtures/violation_aggregate_view.sql", True, "aggregate function call", False),
    ("fixtures/violation_alias_column.sql", True, "ALIAS", False),
    ("fixtures/vacuous_comments_only.sql", True, "ANTI-VACUITY", False),
]


# Mutants of the real schema, each undoing one of ENC-1480's six decisions.
# Generated by substitution rather than committed as files, so they cannot drift
# from the schema they are mutants of — and every substitution is asserted to
# have changed something, because a no-op mutation reads exactly like a passing
# gate.
MUTANTS = [
    ("ingest_windows back to ReplacingMergeTree(completed_at)",
     ("ENGINE = MergeTree\nORDER BY (symbol, kind, from_ts, session_id, updated_at)",
      "ENGINE = ReplacingMergeTree(completed_at)\nORDER BY (symbol, kind, from_ts, session_id)"),
     "ingest_windows ENGINE"),
    ("book_events sort key with the action/order_id discriminator",
     ("CREATE TABLE IF NOT EXISTS gma.book_events", "CREATE TABLE IF NOT EXISTS gma.book_events"),
     "book_events ORDER BY"),
    ("gma.quotes uncommented",
     ("-- CREATE TABLE IF NOT EXISTS gma.quotes",
      "CREATE TABLE IF NOT EXISTS gma.quotes (ts_event DateTime64(9, 'UTC'), symbol LowCardinality(String), bid_px Int64, ask_px Int64) ENGINE = MergeTree ORDER BY (symbol, ts_event);\n-- CREATE TABLE IF NOT EXISTS gma.quotes"),
     "gma.quotes is created"),
    ("a live projection",
     ("CREATE DATABASE IF NOT EXISTS gma;",
      "CREATE DATABASE IF NOT EXISTS gma;\nALTER TABLE gma.trades ADD PROJECTION IF NOT EXISTS p_time_symbol ( SELECT * ORDER BY (ts_event, symbol) );"),
     "a projection is live"),
    ("ingest_windows key back to the session_id-before-clock form",
     ("ORDER BY (symbol, kind, from_ts, to_ts, updated_at)",
      "ORDER BY (symbol, kind, from_ts, session_id, updated_at)"),
     "ingest_windows ORDER BY"),
    ("to_ts dropped from the ingest_windows key",
     ("ORDER BY (symbol, kind, from_ts, to_ts, updated_at)",
      "ORDER BY (symbol, kind, from_ts, updated_at)"),
     "ingest_windows ORDER BY"),
    ("updated_at loses its DEFAULT",
     ("updated_at    DateTime64(3, 'UTC') DEFAULT now64(3),",
      "updated_at    DateTime64(3, 'UTC'),"),
     "DEFAULT now64(3)"),
    ("gaps partitioned on the detection day again",
     ("PARTITION BY toYYYYMMDD(ts_from)", "PARTITION BY toYYYYMMDD(ts_detected)"),
     "gma.gaps PARTITION BY"),
    ("an aggregating rollup appended",
     ("CREATE DATABASE IF NOT EXISTS gma;",
      "CREATE DATABASE IF NOT EXISTS gma;\nCREATE TABLE gma.bars (bucket DateTime64(9,'UTC'), symbol LowCardinality(String), volume AggregateFunction(sum, UInt64)) ENGINE = AggregatingMergeTree ORDER BY (symbol, bucket);"),
     "AggregatingMergeTree"),
]


def _mutant_text(schema_text, label, pair):
    """Apply one mutation, asserting it changed the text."""
    old_s, new_s = pair
    if label.startswith("book_events sort key"):
        # Needs a positional edit: only book_events' ORDER BY, not every
        # table's. Anchored on the sort key followed by SETTINGS, because
        # scanning to the next `;` lands inside a column comment that contains
        # one ("-- Level index for AGGREGATED (L2) feeds; 0 for per-order").
        i = schema_text.index("CREATE TABLE IF NOT EXISTS gma.book_events")
        anchor = "ORDER BY (symbol, ts_event, provider_seq)\nSETTINGS"
        j = schema_text.index(anchor, i)
        out = (schema_text[:j]
               + "ORDER BY (symbol, ts_event, provider_seq, action, order_id)"
                 "\nSETTINGS"
               + schema_text[j + len(anchor):])
        if out == schema_text:
            raise AssertionError("mutation %r was a no-op" % label)
        return out
    if old_s not in schema_text:
        raise AssertionError("mutation %r has no anchor in the schema" % label)
    out = schema_text.replace(old_s, new_s, 1)
    if out == schema_text:
        raise AssertionError("mutation %r was a no-op" % label)
    return out


def mutant_test():
    """Every one of ENC-1480's decisions, undone, must be rejected."""
    import tempfile
    with open(SCHEMA, "r", encoding="utf-8") as fh:
        schema_text = fh.read()
    tmp = tempfile.mkdtemp(prefix="enc1480-mutants-")
    failures = 0
    for label, pair, mention in MUTANTS:
        try:
            text = _mutant_text(schema_text, label, pair)
        except AssertionError as exc:
            print("  [FAIL] %-54s -> could not mutate: %s" % (label, exc))
            failures += 1
            continue
        path = os.path.join(tmp, re.sub(r"\W+", "_", label) + ".sql")
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        findings, _ = scan(path, schema_decisions=True)
        ok = bool(findings)
        if ok and mention:
            ok = any(mention in f for f in findings)
        print("  [%s] %-54s -> %s" % ("ok" if ok else "FAIL", label,
                                      "REJECT" if findings else "accept (WRONG)"))
        for f in findings:
            print("        %s" % f)
        if not ok:
            failures += 1
            if mention:
                print("        expected a finding mentioning %r" % mention)
    return failures


def self_test():
    failures = 0
    for rel, must_fail, mention, decisions in SELF_TESTS:
        path = os.path.join(HERE, rel)
        findings, facts = scan(path, schema_decisions=decisions)
        failed = bool(findings)
        verdict = "REJECT" if failed else "accept"
        want = "REJECT" if must_fail else "accept"
        ok = failed == must_fail
        if ok and mention:
            ok = any(mention in f for f in findings)
        print("  [%s] %-46s -> %-6s (want %s, %d statements, %d comment bytes "
              "stripped)" % ("ok" if ok else "FAIL", rel, verdict, want,
                             facts["statements"], facts["comment_bytes_removed"]))
        if not ok:
            failures += 1
            for f in findings:
                print("        finding: %s" % f)
            if must_fail and not findings:
                print("        expected a finding and got none")
            if mention and not any(mention in f for f in findings):
                print("        expected a finding mentioning %r" % mention)
    return failures


# --------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_lint = sub.add_parser("lint", help="check one DDL file (default: the schema)")
    p_lint.add_argument("file", nargs="?", default=SCHEMA)
    p_lint.add_argument("--no-schema-decisions", action="store_true",
                        help="D2 only; skip the ENC-1480 shape assertions "
                             "(for fixtures and for other DDL files)")
    p_lint.add_argument("--cross-check-aggregates", action="store_true",
                        help="also ask the server for its aggregate list and "
                             "fail if the committed snapshot no longer covers it")
    p_lint.add_argument("--url", default=DEFAULT_URL)

    sub.add_parser("self-test", help="prove the check rejects real violations "
                                     "and accepts comment-only mentions")

    p_ref = sub.add_parser("refresh-aggregates",
                           help="re-derive clickhouse_aggregates.txt from a server")
    p_ref.add_argument("--url", default=DEFAULT_URL)

    args = ap.parse_args(argv)
    try:
        return _run(args)
    except CouldNotRun as exc:
        print("could not run: %s" % exc, file=sys.stderr)
        return 2


def _run(args):
    if args.cmd == "self-test":
        print("check_no_compute self-test — a gate never observed failing has "
              "not been shown to gate:")
        failures = self_test()
        print("\nand every ENC-1480 decision, undone in a copy of the real "
              "schema, must be rejected:")
        failures += mutant_test()
        total = len(SELF_TESTS) + len(MUTANTS)
        if failures:
            print("SELF-TEST FAILED: %d of %d case(s)" % (failures, total))
            return 1
        print("self-test: %d/%d cases behaved as specified" % (total, total))
        return 0

    if args.cmd == "refresh-aggregates":
        try:
            names = server_aggregates(args.url)
            version = ch_query("SELECT version() FORMAT TSVRaw", args.url).strip()
        except Exception as exc:                     # noqa: BLE001
            print("could not run: %s" % exc, file=sys.stderr)
            return 2
        header = []
        with open(AGGREGATES, "r", encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("#") and not line.startswith("# server:"):
                    header.append(line.rstrip("\n"))
                else:
                    break
        with open(AGGREGATES, "w", encoding="utf-8") as fh:
            fh.write("\n".join(header) + "\n")
            fh.write("# server: %s\n" % version)
            fh.write("\n".join(names) + "\n")
        print("refreshed %s: %d aggregate functions from %s"
              % (AGGREGATES, len(names), version))
        return 0

    findings, facts = scan(args.file,
                           schema_decisions=not args.no_schema_decisions)

    if args.cross_check_aggregates:
        try:
            live = set(server_aggregates(args.url))
        except Exception as exc:                     # noqa: BLE001
            print("could not run cross-check: %s" % exc, file=sys.stderr)
            return 2
        missing = sorted(live - set(load_aggregates()))
        if missing:
            findings.append(
                "VOCABULARY: the server knows %d aggregate function(s) the "
                "committed snapshot does not (%s%s). Run `refresh-aggregates`."
                % (len(missing), ", ".join(missing[:8]),
                   ", …" if len(missing) > 8 else ""))
        else:
            print("  cross-check: snapshot covers all %d server aggregates"
                  % len(live))

    print("check_no_compute: %s" % facts["file"])
    print("  %d statements, %d bytes scanned, %d comment bytes stripped first"
          % (facts["statements"], facts["scanned_bytes"],
             facts["comment_bytes_removed"]))
    if facts.get("comment_only_hits"):
        print("  %d forbidden construct(s) appear in COMMENTS only — a scan "
              "without comment stripping would report every one of them"
              % facts["comment_only_hits"])
    if "tables" in facts:
        print("  tables created: %s" % ", ".join(facts["tables"]))
    if findings:
        print("  FINDINGS (%d):" % len(findings))
        for f in findings:
            print("    - %s" % f)
        return 1
    print("  clean: the DDL selects and stores, and computes nothing (SPEC D2)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
