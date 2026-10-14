"""Minimal ClickHouse DDL reader — comment stripping, statement splitting, and a
canonical form for one CREATE TABLE.

ONE parser, used in both directions. `verify` compares `data/schema/clickhouse.sql`
against the server by running THIS parser over the file and over the server's own
`SHOW CREATE TABLE` output, then diffing the canonical forms. A second parser for
the server's dialect would be a second implementation of the thing being checked,
and the two would drift; `SHOW CREATE TABLE` is itself a CREATE TABLE statement,
so there is no reason to have one.

Not a SQL parser. It handles exactly the subset this repo's DDL uses, and raises
on anything it does not understand rather than guessing — a silent mis-parse is
the failure mode that makes a check read as a pass (docs/data-layer.md §5.4,
"anti-vacuity").
"""

import re

# --------------------------------------------------------------------------
# Lexing
# --------------------------------------------------------------------------


def strip_comments(sql: str) -> str:
    """Remove `--` to end-of-line and `/* */` comments, preserving string
    literals and the line structure.

    This is load-bearing for the D2 check and not a convenience: the schema's
    own header documents every construct the check forbids ("no
    AggregatingMergeTree", "no MATERIALIZED or ALIAS column computing a
    value"), so a scan over the raw text reports documentation as a violation.
    Newlines are kept so a reported line number still means something.
    """
    out = []
    i, n = 0, len(sql)
    while i < n:
        c = sql[i]
        if c == "'":                       # string literal — copy verbatim
            out.append(c)
            i += 1
            while i < n:
                out.append(sql[i])
                if sql[i] == "\\":         # escape: copy the next char too
                    if i + 1 < n:
                        out.append(sql[i + 1])
                        i += 2
                        continue
                elif sql[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == "`":                       # quoted identifier — copy verbatim
            out.append(c)
            i += 1
            while i < n:
                out.append(sql[i])
                if sql[i] == "`":
                    i += 1
                    break
                i += 1
            continue
        if sql.startswith("--", i):
            while i < n and sql[i] != "\n":
                i += 1
            continue
        if sql.startswith("/*", i):
            j = sql.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * sql.count("\n", i, j))   # keep line numbering
            i = j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def split_statements(sql: str) -> list:
    """Split comment-stripped SQL on top-level `;`. Returns non-empty,
    whitespace-trimmed statements."""
    stmts, buf = [], []
    i, n = 0, len(sql)
    while i < n:
        c = sql[i]
        if c == "'" or c == "`":
            q = c
            buf.append(c)
            i += 1
            while i < n:
                buf.append(sql[i])
                if sql[i] == "\\" and q == "'":
                    if i + 1 < n:
                        buf.append(sql[i + 1])
                        i += 2
                        continue
                elif sql[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if c == ";":
            stmts.append("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    stmts.append("".join(buf))
    return [s.strip() for s in stmts if s.strip()]


# --------------------------------------------------------------------------
# Canonical text
# --------------------------------------------------------------------------


def norm(s: str) -> str:
    """Canonical spelling of a DDL fragment, so the file and `SHOW CREATE
    TABLE` compare equal where they mean the same thing.

    Collapses whitespace, drops identifier backticks (the server adds them, the
    file does not) and removes whitespace adjacent to `(`, `)` and `,` (the
    server writes `DateTime64(9, 'UTC')`, the file writes
    `DateTime64(9, 'UTC')` with its own spacing). Keeps case, because
    `LowCardinality` and `DateTime64` are case-sensitive type names and a
    difference there is a real difference.
    """
    s = s.replace("`", "")
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"\s*([(),])\s*", r"\1", s)
    return s


def _split_top_level(s: str, sep: str = ",") -> list:
    """Split on `sep` at paren depth 0, respecting string literals."""
    parts, buf, depth = [], [], 0
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "'":
            buf.append(c)
            i += 1
            while i < n:
                buf.append(s[i])
                if s[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    parts.append("".join(buf))
    return [p.strip() for p in parts if p.strip()]


def _matching_paren(s: str, open_idx: int) -> int:
    depth = 0
    i, n = open_idx, len(s)
    while i < n:
        c = s[i]
        if c == "'":
            i += 1
            while i < n and s[i] != "'":
                i += 1
            i += 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced parentheses")


# Clause keywords that terminate the previous clause in a CREATE TABLE tail.
_TAIL_KEYWORDS = [
    "ENGINE", "PARTITION BY", "PRIMARY KEY", "ORDER BY", "SAMPLE BY",
    "TTL", "SETTINGS", "COMMENT",
]
_TAIL_RE = re.compile(
    r"\b(" + "|".join(k.replace(" ", r"\s+") for k in _TAIL_KEYWORDS) + r")\b",
    re.IGNORECASE,
)

_CREATE_TABLE_RE = re.compile(
    r"^CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z0-9_.`]+)\s*\(",
    re.IGNORECASE,
)


def is_create_table(stmt: str) -> bool:
    return _CREATE_TABLE_RE.match(stmt.strip()) is not None


def parse_create_table(stmt: str) -> dict:
    """Canonical form of one CREATE TABLE: table name, flattened column list,
    index list, and the tail clauses.

    `Nested(...)` is expanded exactly as ClickHouse stores it — one
    `Array(<type>)` column per field, named `<col>.<field>`, each inheriting the
    Nested column's CODEC. Measured against the running server, not assumed:
    a per-field CODEC inside Nested is a syntax error, and the column-level
    CODEC lands on every flattened array.
    """
    stmt = stmt.strip()
    m = _CREATE_TABLE_RE.match(stmt)
    if not m:
        raise ValueError("not a CREATE TABLE statement: %r" % stmt[:60])
    table = m.group(1).replace("`", "")
    open_idx = stmt.index("(", m.end() - 1)
    close_idx = _matching_paren(stmt, open_idx)
    body = stmt[open_idx + 1:close_idx]
    tail = stmt[close_idx + 1:]

    columns, indexes = [], []
    for item in _split_top_level(body):
        head = item.split(None, 1)[0].upper()
        if head in ("INDEX", "PROJECTION", "CONSTRAINT"):
            indexes.append(norm(item))
            continue
        columns.extend(_parse_column(item))

    clauses = {}
    marks = [(mm.start(), mm.group(1).upper()) for mm in _TAIL_RE.finditer(tail)]
    marks = [(pos, re.sub(r"\s+", " ", kw)) for pos, kw in marks]
    for idx, (pos, kw) in enumerate(marks):
        end = marks[idx + 1][0] if idx + 1 < len(marks) else len(tail)
        value = tail[pos:end]
        value = value[len(kw):].lstrip() if value.upper().startswith(kw) else value
        value = re.sub(r"^\s*=\s*", "", value)
        clauses[kw] = norm(value)

    if "SETTINGS" in clauses:
        settings = {}
        for part in _split_top_level(clauses["SETTINGS"]):
            if "=" not in part:
                continue
            k, v = norm(part).split("=", 1)
            settings[k.strip()] = v.strip()
        clauses["SETTINGS"] = settings

    return {"table": table, "columns": columns, "indexes": indexes,
            "clauses": clauses}


_CODEC_RE = re.compile(r"\bCODEC\s*\(", re.IGNORECASE)


def _parse_column(item: str) -> list:
    """One column definition -> list of (name, type, codec) triples.

    A list rather than a single triple only because of `Nested`."""
    bits = item.split(None, 1)
    name = bits[0].replace("`", "")
    rest = bits[1].strip() if len(bits) > 1 else ""

    codec = ""
    cm = _CODEC_RE.search(rest)
    if cm:
        cclose = _matching_paren(rest, rest.index("(", cm.end() - 1))
        codec = norm(rest[cm.start():cclose + 1])
        rest = (rest[:cm.start()] + rest[cclose + 1:]).strip()
    ctype = norm(rest)

    if ctype.upper().startswith("NESTED("):
        inner = ctype[ctype.index("(") + 1:_matching_paren(ctype, ctype.index("("))]
        out = []
        for f in _split_top_level(inner):
            fname, ftype = f.split(None, 1) if " " in f else (f, "")
            # Nested fields are stored with no space after the comma in the
            # column list, so re-split on the type boundary the normalizer made.
            if not ftype:
                fname, ftype = re.match(r"([A-Za-z0-9_]+)(.*)", f).groups()
            out.append((name + "." + fname.strip(), "Array(%s)" % norm(ftype), codec))
        return out
    return [(name, ctype, codec)]


def read_statements(path: str):
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()
    return split_statements(strip_comments(raw)), raw
