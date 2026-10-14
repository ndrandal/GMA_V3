#!/usr/bin/env python3
"""Assert gma_engine has not acquired a data-layer dependency.  (ENC-1481)

Design: specs/2026-10-14-gma-data-layer/SPEC.md D14;
        docs/data-layer.md §4 rule 6.

THE INVARIANT, in the wording that actually matters:

    gma_engine must not acquire a dependency on Arrow, the ClickHouse client,
    or the gma_data library target.

Not "no gma/data header is reachable from engine code" -- that phrasing is
wrong, and the distinction is load-bearing.  `include/gma/data/Record.hpp`
lives inside the engine's own include tree on purpose: ENC-1486 adds
`Dispatcher::onRecord(const data::TickRecord&)`, and Dispatcher IS engine code,
so the engine has to be able to name a record.  What it must not be able to
name is Arrow.  The PODs are Arrow-free; the schemas, in `data/`, are not.

WHY THIS IS A SCRIPT AND NOT A SENTENCE IN A COMMIT MESSAGE
This is the invariant in the design most likely to rot silently, because every
way of breaking it still compiles and still passes every existing test.  Adding
`gma::arrow` to gma_engine's link line makes the LIVE-ONLY VARIANT of SPEC
D14's client profile impossible, and nothing anywhere would have noticed.
(D14's wording is "live-only *or* reads a central L1 over the wire with no
local store".  The second variant does ship gma_data -- it needs Arrow IPC --
so what the invariant protects is the first one, plus the general property that
gma_engine builds whether or not the data layer is in the build.  "The client
profile omits gma_data" is true of one variant and not of D14.)

FOUR CHECKS, each closing a different route in:

  A  textual   no engine translation unit includes an Arrow / Parquet /
               ClickHouse / DuckDB header, nor a header that exists only under
               `data/include` or `connectors/*/include`.
  B  include   gma_engine's include directories, restricted to paths inside
               the repo, are exactly {<root>/include}.
  C  link      nothing matching gma_data / arrow / parquet / clickhouse is
               reachable from gma_engine's link libraries — TRANSITIVELY.
  D  profile   nothing matching them is reachable from gma_server's either,
               which is what makes the LIVE-ONLY VARIANT of SPEC D14's client
               profile (GMA_BUILD_DATA=OFF) actually buildable rather than
               nominally so.

C AND D WALK THE CLOSURE, AND HAVE TO.  They read a *link graph* out of the
manifest, not one target's own property, because the direct property is the
wrong place to look.  `gma_server` links the `gma` umbrella and nothing else
(`link.gma_server=gma|Boost::system|Threads::Threads|RapidJSON::rapidjson`),
so a single `target_link_libraries(gma INTERFACE gma_data)` -- or `gma::arrow`
on `gma_connector_market`, which `gma` also pulls in -- makes
`GMA_BUILD_DATA=OFF` unlinkable while gma_engine's and gma_server's OWN
properties stay spotless.  That is precisely the "one line and nothing would
say so" failure this file exists to close, so a one-level check does not close
it.  CMake emits one row per first-party target (`link.<target>` /
`interface_link.<target>`); anything not named by a row is a leaf and is only
pattern-matched.  The selftest pins both the umbrella case and, as a labelled
contrast, the fact that a one-level read would have passed it.

POSITIVE CONTROL.  When gma_data is in the build, the closure from gma_data
MUST trip a forbidden pattern -- it links `gma::arrow` by construction.  If it
does not, the patterns match nothing in this build and a clean verdict about
gma_engine is a verdict about a search that cannot find anything.  (The
workspace-root grep that reaches none of the seven repos is the same bug.)
With GMA_BUILD_DATA=OFF there is no gma_data target and no control available;
that is stated in the summary rather than passed over.

`src/main.cpp` IS NOT ENGINE CODE and is checked by a weaker rule.  The root
CMakeLists removes it from gma_engine's sources and compiles it into
`gma_server` against the `gma` umbrella, so it legitimately includes
`gma/market/MarketConnector.hpp` -- which the first run of this script flagged,
correctly, as a connector header reached from `src/`.  It still must not reach
Arrow or `data/`, because gma_server has to link with GMA_BUILD_DATA=OFF.

The file list for check A comes from CMake's own
`$<TARGET_PROPERTY:gma_engine,SOURCES>`, not from this script re-deriving the
glob.  A second derivation of the same set is a second thing to keep in sync,
and the one that drifts is always the copy.  The filesystem walk is kept as a
cross-check in the other direction: it fails if gma_engine compiles a file from
outside src/ and include/.

ANTI-VACUITY.  Every check asserts its own input is non-empty.  A scan of zero
files and a property list of zero entries both pass every predicate above while
proving nothing, and that reads exactly like a clean run -- the failure mode
this repo has been bitten by more than once (ENC-1289's expected-failure
machinery, the lint anti-vacuity rule in docs/data-layer.md §5.4, and the
workspace-root grep that reaches none of the seven repos).  An empty input is
an ERROR here, not a pass.

SELF-TEST.  `--selftest` runs each predicate against an input that violates it
and fails unless the violation is detected.  So the claim "this check would
catch it" is itself checked, every ctest run, rather than being a claim.
"""

from __future__ import annotations

import argparse
import os
import re
import sys

# Headers that must never be reachable from engine code.  Matched as a prefix
# of the quoted include path.
FORBIDDEN_INCLUDE_PREFIXES = (
    "arrow/",
    "arrow_",
    "parquet/",
    "gandiva/",
    "clickhouse/",
    "clickhouse-cpp/",
    "duckdb",
)

# Library names that must never appear in gma_engine's link libraries.
FORBIDDEN_LINK_PATTERNS = (
    re.compile(r"(?:^|[:/])gma_data$"),
    re.compile(r"arrow", re.IGNORECASE),
    re.compile(r"parquet", re.IGNORECASE),
    re.compile(r"clickhouse", re.IGNORECASE),
    re.compile(r"duckdb", re.IGNORECASE),
)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

# Floors for the anti-vacuity assertions.  The margin has to be real, not
# claimed: the engine is **99** source/header files once src/main.cpp is
# excluded (measured 2026-10-14 -- `find src include -type f` is 101, of which
# one is src/util/gma.conf and one is the composition root, and the gate prints
# the 99 it actually scanned).  The 50 that stood here was advertised in a
# comment as "well under the real count (the engine was ~190 files)"; 190 was
# the whole repo's .cpp/.hpp count, so the floor was in fact HALF the true
# number and a traversal that silently found half the engine would have passed.
# 80 is a ~19% margin -- enough for ordinary deletion, not enough to hide a
# broken walk.
MIN_ENGINE_FILES = 80
MIN_ENGINE_INCLUDE_DIRS = 1


def read_manifest(path: str) -> dict[str, str]:
    """Parse the key=value file CMake generates with the resolved properties."""
    out: dict[str, str] = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, _, value = line.partition("=")
            out[key.strip()] = value.strip()
    return out


def split_list(value: str) -> list[str]:
    # CMake's $<JOIN:...,|> output.  An empty property yields "".
    return [part for part in (p.strip() for p in value.split("|")) if part]


def dedupe(items: list[str]) -> list[str]:
    """Order-preserving dedupe.

    INCLUDE_DIRECTORIES and INTERFACE_INCLUDE_DIRECTORIES overlap almost
    entirely for a PUBLIC include dir, so without this every include-path
    violation is reported twice and the count in the summary is wrong.
    """
    seen: set[str] = set()
    out: list[str] = []
    for item in items:
        if item not in seen:
            seen.add(item)
            out.append(item)
    return out


SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".cc", ".cxx", ".ipp")

# Compiled into gma_server, not gma_engine -- see the docstring.
COMPOSITION_ROOT = os.path.join("src", "main.cpp")


def walk_engine_trees(root: str) -> list[str]:
    """Every source/header under the two trees gma_engine is globbed from.

    Not connectors/, not data/, not tests/.  The composition root is excluded
    because CMake excludes it from the target.
    """
    found: list[str] = []
    for sub in ("src", "include"):
        base = os.path.join(root, sub)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build")]
            for name in filenames:
                if name.endswith(SOURCE_SUFFIXES):
                    found.append(os.path.realpath(os.path.join(dirpath, name)))
    composition = os.path.realpath(os.path.join(root, COMPOSITION_ROOT))
    return sorted(f for f in found if f != composition)


def check_cmake_sources_stay_in_the_engine_trees(
        root: str, cmake_sources: list[str], walked: set[str]) -> list[str]:
    """Nothing from outside src/ and include/ is compiled into gma_engine.

    The other direction of check A: if gma_engine ever started compiling a file
    from data/ or connectors/, the walk above would never see it and A would
    pass while the layering had already been broken.
    """
    composition = os.path.realpath(os.path.join(root, COMPOSITION_ROOT))
    problems: list[str] = []
    for src in cmake_sources:
        real = os.path.realpath(src)
        if not real.endswith(SOURCE_SUFFIXES):
            continue
        if real == composition:
            problems.append(
                "gma_engine compiles src/main.cpp -- the composition root "
                "belongs to gma_server, and pulling it into the engine library "
                "drags the market connector in with it")
            continue
        if real not in walked:
            problems.append(
                f"gma_engine compiles {os.path.relpath(real, root)}, which is "
                f"outside src/ and include/ -- engine sources must come from "
                f"the engine's own trees")
    return problems


def non_engine_header_roots(root: str) -> list[str]:
    """Include roots that belong to gma_data and the connectors, not the engine."""
    roots = [os.path.join(root, "data", "include")]
    connectors = os.path.join(root, "connectors")
    if os.path.isdir(connectors):
        for name in sorted(os.listdir(connectors)):
            candidate = os.path.join(connectors, name, "include")
            if os.path.isdir(candidate):
                roots.append(candidate)
    return [r for r in roots if os.path.isdir(r)]


def check_a_includes(root: str, sources: list[str], foreign_roots: list[str],
                     label: str = "engine code") -> list[str]:
    """No file in `sources` includes a forbidden header or a foreign-only one.

    `foreign_roots` is the set of include trees this file may not reach into.
    For engine proper that is data/include AND every connector; for the
    composition root it is data/include only, since main.cpp's whole job is to
    wire connectors up.
    """
    engine_include = os.path.join(root, "include")
    problems: list[str] = []
    for path in sources:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                lines = fh.readlines()
        except OSError as exc:  # pragma: no cover - unreadable file is a problem
            problems.append(f"{path}: cannot read ({exc})")
            continue
        for lineno, line in enumerate(lines, start=1):
            m = INCLUDE_RE.match(line)
            if not m:
                continue
            inc = m.group(1)
            low = inc.lower()
            for bad in FORBIDDEN_INCLUDE_PREFIXES:
                if low.startswith(bad):
                    problems.append(
                        f"{os.path.relpath(path, root)}:{lineno}: {label} "
                        f"includes <{inc}> -- Arrow/ClickHouse must never become "
                        f"a gma_engine or gma_server dependency (SPEC D14)")
                    break
            else:
                # A header that resolves under data/ or a connector but NOT
                # under the engine's own include/ is a layering violation.
                # `gma/data/Record.hpp` resolves under include/, so it passes;
                # `gma/data/Schema.hpp` does not, so it does not.
                if os.path.exists(os.path.join(engine_include, inc)):
                    continue
                for froot in foreign_roots:
                    if os.path.exists(os.path.join(froot, inc)):
                        problems.append(
                            f"{os.path.relpath(path, root)}:{lineno}: {label} "
                            f"includes \"{inc}\", which exists only under "
                            f"{os.path.relpath(froot, root)}/ -- the engine may not "
                            f"depend on gma_data or on a connector")
                        break
    return problems


def check_b_include_dirs(root: str, dirs: list[str]) -> list[str]:
    """gma_engine's in-repo include dirs are exactly {<root>/include}."""
    allowed = os.path.realpath(os.path.join(root, "include"))
    real_root = os.path.realpath(root)
    problems: list[str] = []
    in_repo: list[str] = []
    for d in dirs:
        real = os.path.realpath(d)
        if real == real_root or real.startswith(real_root + os.sep):
            in_repo.append(real)
    for real in in_repo:
        if real != allowed and not real.startswith(allowed + os.sep):
            problems.append(
                f"gma_engine include directory {os.path.relpath(real, root)} is "
                f"inside the repo but is not include/ -- the engine's include "
                f"path must expose engine headers only")
    if allowed not in in_repo:
        problems.append(
            "gma_engine's include directories do not contain include/ at all; "
            "the manifest is wrong or the target was restructured")
    return problems


# CMake leaves generator-expression wrappers in LINK_LIBRARIES --
# `$<LINK_ONLY:Boost::headers>` is the common one -- and appends a
# `::@(<directory-id>)` marker to an INTERFACE_LINK_LIBRARIES entry recorded
# from another directory scope.  Neither is part of the target name, and an
# unstripped name is a graph node that never matches a `link.<target>` row, so
# the walk would stop one edge short of the thing it is looking for.
GENEX_RE = re.compile(r"^\$<[A-Za-z_0-9]+:(.*)>$")
DIRSCOPE_RE = re.compile(r"::@\(?[^)]*\)?$")


def link_item_name(item: str) -> str:
    """The bare target/library name behind CMake's link-entry decorations."""
    name = item.strip()
    while True:
        m = GENEX_RE.match(name)
        if not m:
            break
        name = m.group(1).strip()
    return DIRSCOPE_RE.sub("", name).strip()


def cmake_true(value: str) -> bool:
    """CMake's truthiness, for the boolean strings the manifest carries.

    `option()` does not normalise, so `-DGMA_BUILD_DATA=1` reaches the manifest
    as "1", not "ON".  Anything unrecognised is false, which is also CMake's
    rule and is the safe direction: an unparseable value then DISAGREES with
    the link graph and is reported rather than quietly believed.
    """
    v = value.strip().upper()
    if v in ("ON", "YES", "TRUE", "Y"):
        return True
    if v in ("OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""):
        return False
    try:
        return int(v) != 0
    except ValueError:
        return False


def parse_link_graph(manifest: dict[str, str]) -> dict[str, list[str]]:
    """target -> its link libraries, from the manifest's `link.<target>` rows.

    LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES are merged per target: for the
    invariant it does not matter which of the two carries the forbidden entry,
    only that the link line ends up with it.
    """
    graph: dict[str, list[str]] = {}
    for key, value in manifest.items():
        for prefix in ("link.", "interface_link."):
            if key.startswith(prefix):
                graph.setdefault(key[len(prefix):], []).extend(split_list(value))
                break
    return {t: dedupe(libs) for t, libs in graph.items()}


def link_closure(graph: dict[str, list[str]],
                 root: str) -> list[tuple[str, tuple[str, ...]]]:
    """Every library reachable from `root`, with the chain that reaches it.

    Breadth-first so the reported chain is a shortest one, and cycle-safe: a
    name is expanded at most once.  A name with no row in the graph is a leaf
    (a system library, an imported target) and is matched but not expanded.
    """
    out: list[tuple[str, tuple[str, ...]]] = []
    seen = {root}
    queue: list[tuple[str, tuple[str, ...]]] = [(root, (root,))]
    while queue:
        target, path = queue.pop(0)
        for raw in graph.get(target, ()):
            name = link_item_name(raw)
            if not name or name in seen:
                continue
            seen.add(name)
            chain = path + (name,)
            out.append((name, chain))
            if name in graph:
                queue.append((name, chain))
    return out


def forbidden_links(graph: dict[str, list[str]], root: str) -> list[str]:
    """Checks C and D: nothing forbidden anywhere in `root`'s link closure.

    Transitive, not direct -- see the docstring. The chain is printed because
    the fix is almost never on the root's own link line.
    """
    problems: list[str] = []
    for name, chain in link_closure(graph, root):
        for pattern in FORBIDDEN_LINK_PATTERNS:
            if pattern.search(name):
                problems.append(
                    f"{root} links '{name}' via {' -> '.join(chain)} -- Arrow, "
                    f"the ClickHouse client and gma_data must never be "
                    f"reachable from {root}'s link line, because SPEC D14's "
                    f"live-only client variant omits gma_data entirely and "
                    f"must still link")
                break
    return problems


def selftest() -> int:
    """Each predicate, shown failing on an input that violates it.

    Without this, a refactor that broke a predicate would turn the whole check
    into a green no-op and nothing would say so.
    """
    import tempfile

    failures: list[str] = []

    def expect_detected(name: str, problems: list[str]) -> None:
        if not problems:
            failures.append(
                f"SELFTEST: {name} did NOT flag a violating input -- the check "
                f"is a no-op and would pass a real violation")

    def expect_clean(name: str, problems: list[str]) -> None:
        if problems:
            failures.append(
                f"SELFTEST: {name} flagged a LEGAL input: {problems}")

    with tempfile.TemporaryDirectory(prefix="enc1481-isolation-") as tmp:
        # A miniature repo with the real shape: an engine include tree that
        # legitimately holds gma/data/Record.hpp, and a data/include tree that
        # holds gma/data/Schema.hpp.
        os.makedirs(os.path.join(tmp, "include", "gma", "data"))
        os.makedirs(os.path.join(tmp, "data", "include", "gma", "data"))
        os.makedirs(os.path.join(tmp, "src", "core"))
        open(os.path.join(tmp, "include", "gma", "data", "Record.hpp"), "w").close()
        open(os.path.join(tmp, "data", "include", "gma", "data", "Schema.hpp"), "w").close()

        os.makedirs(os.path.join(tmp, "connectors", "market", "include", "gma", "market"))
        open(os.path.join(tmp, "connectors", "market", "include", "gma",
                          "market", "MarketConnector.hpp"), "w").close()

        good = os.path.join(tmp, "src", "core", "Ok.cpp")
        with open(good, "w", encoding="utf-8") as fh:
            fh.write('#include "gma/data/Record.hpp"\n#include <vector>\n')

        foreign = non_engine_header_roots(tmp)
        expect_clean("A/legal-Record.hpp", check_a_includes(tmp, [good], foreign))

        # The composition root: it may reach a connector, it may not reach
        # Arrow or data/. This is the distinction the first real run of this
        # script surfaced, so both halves of it are pinned here.
        main_cpp = os.path.join(tmp, "src", "main.cpp")
        with open(main_cpp, "w", encoding="utf-8") as fh:
            fh.write('#include "gma/market/MarketConnector.hpp"\n')
        data_only = [r for r in foreign if r.endswith(os.path.join("data", "include"))]
        expect_clean("A/composition-root-may-use-a-connector",
                     check_a_includes(tmp, [main_cpp], data_only, "composition root"))
        expect_detected("A/composition-root-may-not-use-a-connector-as-engine",
                        check_a_includes(tmp, [main_cpp], foreign))
        main_bad = os.path.join(tmp, "src", "main_bad.cpp")
        with open(main_bad, "w", encoding="utf-8") as fh:
            fh.write('#include <arrow/api.h>\n')
        expect_detected("A/composition-root-may-not-use-arrow",
                        check_a_includes(tmp, [main_bad], data_only, "composition root"))

        # The walk must exclude the composition root, and the cross-check must
        # notice if CMake ever compiles it into gma_engine anyway.
        walked = set(walk_engine_trees(tmp))
        if os.path.realpath(main_cpp) in walked:
            failures.append("SELFTEST: walk_engine_trees did not exclude src/main.cpp")
        if os.path.realpath(good) not in walked:
            failures.append("SELFTEST: walk_engine_trees missed a real engine source")
        expect_detected("cross/main.cpp-in-engine-target",
                        check_cmake_sources_stay_in_the_engine_trees(
                            tmp, [main_cpp], walked))
        expect_detected("cross/data-source-in-engine-target",
                        check_cmake_sources_stay_in_the_engine_trees(
                            tmp, [os.path.join(tmp, "data", "src", "Schema.cpp")], walked))
        expect_clean("cross/legal",
                     check_cmake_sources_stay_in_the_engine_trees(tmp, [good], walked))

        bad_arrow = os.path.join(tmp, "src", "core", "BadArrow.cpp")
        with open(bad_arrow, "w", encoding="utf-8") as fh:
            fh.write("#include <arrow/api.h>\n")
        expect_detected("A/arrow-include",
                        check_a_includes(tmp, [bad_arrow], foreign))

        bad_schema = os.path.join(tmp, "src", "core", "BadSchema.cpp")
        with open(bad_schema, "w", encoding="utf-8") as fh:
            fh.write('#include "gma/data/Schema.hpp"\n')
        expect_detected("A/data-only-header",
                        check_a_includes(tmp, [bad_schema], foreign))

        expect_clean("B/legal",
                     check_b_include_dirs(tmp, [os.path.join(tmp, "include")]))
        expect_detected("B/data-include-on-engine-path",
                        check_b_include_dirs(
                            tmp, [os.path.join(tmp, "include"),
                                  os.path.join(tmp, "data", "include")]))
        expect_detected("B/include-missing", check_b_include_dirs(tmp, ["/usr/include"]))

        # --- C and D: the link closure ----------------------------------
        # The real shape of this repo's link graph. gma_server links the `gma`
        # umbrella and nothing else, which is the whole reason these checks
        # cannot read one property and stop.
        legal = {
            "gma_server": ["gma", "Boost::system", "Threads::Threads",
                           "RapidJSON::rapidjson"],
            "gma": ["gma_engine", "gma_connector_market"],
            "gma_engine": ["Boost::system", "Threads::Threads",
                           "RapidJSON::rapidjson", "$<LINK_ONLY:Boost::headers>"],
            "gma_connector_market": ["gma_engine"],
            "gma_connector_synthetic": ["gma_engine"],
        }
        expect_clean("C/closure-legal", forbidden_links(legal, "gma_engine"))
        expect_clean("D/closure-legal", forbidden_links(legal, "gma_server"))

        # THE CASE THAT MOTIVATED THE CLOSURE. One line on the umbrella makes
        # GMA_BUILD_DATA=OFF unlinkable; neither gma_engine's nor gma_server's
        # own property changes at all.
        umbrella = dict(legal)
        umbrella["gma"] = ["gma_engine", "gma_connector_market", "gma_data"]
        expect_detected("D/umbrella-links-gma_data",
                        forbidden_links(umbrella, "gma_server"))
        expect_clean("C/umbrella-does-not-touch-the-engine",
                     forbidden_links(umbrella, "gma_engine"))

        # CONTRAST, not a gate: the review finding in one assertion. A check
        # that reads only gma_server's OWN link libraries sees
        # ["gma", "Boost::system", ...] and passes the graph above. If this
        # ever stops being true the contrast is stale and should be deleted.
        direct_only = [link_item_name(x) for x in umbrella["gma_server"]]
        if any(p.search(n) for n in direct_only for p in FORBIDDEN_LINK_PATTERNS):
            failures.append(
                "SELFTEST: the one-level contrast case is no longer one-level "
                "-- D/umbrella-links-gma_data no longer demonstrates why the "
                "closure walk exists")

        # The second route in: Arrow on the market connector. It does NOT make
        # the engine depend on Arrow (C stays clean, correctly) but it does
        # make the live-only server unlinkable, so D must redden.
        connector_arrow = dict(legal)
        connector_arrow["gma_connector_market"] = ["gma_engine", "gma::arrow"]
        expect_detected("D/connector-links-arrow",
                        forbidden_links(connector_arrow, "gma_server"))
        expect_clean("C/connector-arrow-is-not-an-engine-dependency",
                     forbidden_links(connector_arrow, "gma_engine"))

        # Direct links still caught, one per forbidden pattern.
        for name in ("gma_data", "gma::arrow", "arrow_static", "parquet",
                     "clickhouse-cpp", "duckdb"):
            expect_detected(f"C/direct-{name}",
                            forbidden_links({"gma_engine": ["Threads::Threads",
                                                            name]},
                                            "gma_engine"))

        # Wrapped entries must be unwrapped, or the walk stops an edge short.
        expect_detected("C/genex-wrapped",
                        forbidden_links(
                            {"gma_engine": ["$<LINK_ONLY:gma_data>"]},
                            "gma_engine"))
        expect_detected("C/directory-scoped-interface-entry",
                        forbidden_links(
                            {"gma_server": ["gma::@(0x55f0)"],
                             "gma": ["gma_data"]},
                            "gma_server"))

        # A cycle must terminate rather than hang, and must still report.
        expect_detected("closure/cycle-safe",
                        forbidden_links({"a": ["b"], "b": ["a", "gma_data"]}, "a"))

        # The positive control itself: gma_data's closure must trip a pattern,
        # since that is what main() asserts about the live build.
        expect_detected("control/gma_data-reaches-arrow",
                        forbidden_links(
                            {"gma_data": ["gma_engine", "gma::arrow"],
                             "gma_engine": ["Threads::Threads"]},
                            "gma_data"))

    if failures:
        for f in failures:
            print(f, file=sys.stderr)
        print(f"\nSELFTEST FAILED: {len(failures)} predicate(s) are not gating.",
              file=sys.stderr)
        return 1
    print("selftest: all predicates detect their violation and pass legal input")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest",
                    help="key=value file generated by CMake with gma_engine's "
                         "resolved include dirs and sources, plus one "
                         "link.<target> / interface_link.<target> row per "
                         "first-party target (the link graph checks C and D "
                         "walk)")
    ap.add_argument("--selftest", action="store_true",
                    help="verify each predicate flags a violating input")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.manifest:
        print("error: --manifest is required (or pass --selftest)", file=sys.stderr)
        return 2

    manifest = read_manifest(args.manifest)
    root = manifest.get("root", "")
    if not root or not os.path.isdir(root):
        print(f"error: manifest 'root' is missing or not a directory: {root!r}",
              file=sys.stderr)
        return 2

    include_dirs = dedupe(
        split_list(manifest.get("engine_include_dirs", "")) +
        split_list(manifest.get("engine_interface_include_dirs", "")))
    graph = parse_link_graph(manifest)
    link_libs = graph.get("gma_engine", [])
    server_libs = graph.get("gma_server", [])
    engine_closure = link_closure(graph, "gma_engine")
    server_closure = link_closure(graph, "gma_server")
    cmake_sources = dedupe(split_list(manifest.get("engine_cmake_sources", "")))
    data_enabled = manifest.get("gma_data_enabled", "?")

    walked = walk_engine_trees(root)
    walked_set = set(walked)
    # Scan the union: CMake's list is authoritative for what gma_engine
    # compiles, and the walk catches a header CMake's glob pattern happens not
    # to match.
    sources = sorted(walked_set | {os.path.realpath(s) for s in cmake_sources
                                   if s.endswith(SOURCE_SUFFIXES)
                                   and os.path.realpath(s) != os.path.realpath(
                                       os.path.join(root, COMPOSITION_ROOT))})
    foreign_roots = non_engine_header_roots(root)
    data_only_roots = [r for r in foreign_roots
                       if r.endswith(os.path.join("data", "include"))]
    composition_root = os.path.join(root, COMPOSITION_ROOT)

    # --- anti-vacuity: an empty input is a failure, not a pass --------------
    vacuous: list[str] = []
    if len(sources) < MIN_ENGINE_FILES:
        vacuous.append(
            f"scanned only {len(sources)} engine source/header files (floor "
            f"{MIN_ENGINE_FILES}) -- the traversal found nothing, so a clean "
            f"result proves nothing")
    if len(include_dirs) < MIN_ENGINE_INCLUDE_DIRS:
        vacuous.append(
            f"gma_engine reported {len(include_dirs)} include directories -- "
            f"the CMake manifest did not resolve")
    if not link_libs:
        vacuous.append(
            "gma_engine reported no link libraries -- the CMake manifest did "
            "not resolve (it links at least Threads::Threads)")
    if not foreign_roots:
        vacuous.append(
            "found no non-engine include roots (data/include, "
            "connectors/*/include) -- check A cannot detect a layering "
            "violation it has no tree to resolve against")
    if not cmake_sources:
        vacuous.append(
            "the manifest lists no gma_engine sources -- the generator "
            "expression did not resolve, so the cross-check is inert")
    if not server_libs:
        vacuous.append(
            "the manifest lists no gma_server link libraries -- check D is "
            "inert (gma_server links at least the `gma` umbrella)")
    if "gma" not in graph:
        vacuous.append(
            "the link graph has no row for the `gma` umbrella -- the manifest "
            "is one-level-deep again, and the umbrella is exactly where the "
            "review found the hole (CMake must emit link.<target> for every "
            "first-party target)")
    if not any(name == "gma_engine" for name, _ in server_closure):
        vacuous.append(
            "gma_server's link closure does not reach gma_engine -- it goes "
            "gma_server -> gma -> gma_engine, so a closure that does not "
            "contain gma_engine never traversed an edge and check D is a "
            "one-level check wearing a closure's name")
    # POSITIVE CONTROL: the patterns must be shown to match something in THIS
    # build before a clean engine verdict means anything.
    #
    # Driven by the GRAPH, not by the gma_data_enabled string, and the two are
    # cross-checked.  CMake writes whichever truthy spelling the user passed --
    # `-DGMA_BUILD_DATA=1` yields "1" -- so a bare `== "ON"` would report the
    # control as unavailable, in those words, in a build that does have
    # gma_data.  A control that announces its own absence incorrectly is worse
    # than no control.
    have_gma_data = "gma_data" in graph
    if have_gma_data:
        hits = forbidden_links(graph, "gma_data")
        if not hits:
            vacuous.append(
                "gma_data's own link closure trips NO forbidden pattern, but it "
                "links gma::arrow by construction -- the patterns match nothing "
                "in this build, so checks C and D cannot detect a violation and "
                "a pass proves nothing")
            control = "FAILED (gma_data's closure trips no forbidden pattern)"
        else:
            control = f"ok ({len(hits)} forbidden link(s) seen from gma_data)"
    else:
        control = (f"unavailable -- gma_data is not a target in this build "
                   f"(gma_data_enabled={data_enabled!r})")
    if have_gma_data != cmake_true(data_enabled):
        vacuous.append(
            f"gma_data_enabled={data_enabled!r} but the manifest "
            f"{'has' if have_gma_data else 'has no'} link.gma_data row -- the "
            f"two disagree, so the positive control's verdict describes a "
            f"build this manifest is not")
    if not os.path.isfile(composition_root):
        vacuous.append(
            f"{COMPOSITION_ROOT} does not exist -- the composition-root rule "
            f"has nothing to check, and the walk's exclusion is hiding "
            f"nothing rather than hiding main.cpp")

    problems = (check_a_includes(root, sources, foreign_roots) +
                (check_a_includes(root, [composition_root], data_only_roots,
                                  "composition root (src/main.cpp)")
                 if os.path.isfile(composition_root) else []) +
                check_cmake_sources_stay_in_the_engine_trees(
                    root, cmake_sources, walked_set) +
                check_b_include_dirs(root, include_dirs) +
                forbidden_links(graph, "gma_engine") +
                forbidden_links(graph, "gma_server"))

    print(f"gma_engine isolation (ENC-1481, SPEC D14)")
    print(f"  root            : {root}")
    print(f"  engine files    : {len(sources)} "
          f"({len(cmake_sources)} from CMake, {len(walked)} walked)")
    print(f"  include dirs    : {len(include_dirs)}")
    print(f"  link libraries  : {len(link_libs)} engine, {len(server_libs)} server "
          f"(direct)")
    print(f"  link closure    : {len(engine_closure)} from gma_engine, "
          f"{len(server_closure)} from gma_server, over "
          f"{len(graph)} graphed target(s): {sorted(graph)}")
    print(f"  positive control: {control}")
    print(f"  foreign roots   : {[os.path.relpath(r, root) for r in foreign_roots]}")
    print(f"  gma_data built  : {data_enabled}")

    if vacuous:
        print("\nVACUOUS INPUT -- refusing to report a pass:", file=sys.stderr)
        for v in vacuous:
            print(f"  ! {v}", file=sys.stderr)
        return 1

    if problems:
        print(f"\nFAILED: {len(problems)} isolation violation(s):", file=sys.stderr)
        for p in problems:
            print(f"  ! {p}", file=sys.stderr)
        print("\nArrow and the ClickHouse client must never become gma_engine "
              "dependencies (SPEC D14, docs/data-layer.md §4 rule 6): the "
              "live-only variant of D14's client profile omits gma_data "
              "entirely and must still link.", file=sys.stderr)
        return 1

    print("\nOK: no Arrow/ClickHouse include in engine code or the composition "
          "root, no data/ or connector header reached from the engine, engine "
          "include path is include/ only, and no data-layer library anywhere "
          "in gma_engine's or gma_server's TRANSITIVE link closure.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
