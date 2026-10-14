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
               nominally so.  `gma_tests` is checked on the same rule, because
               data/CMakeLists.txt names "the engine test suite is built
               against an Arrow-free include path" as the thing being
               protected, and a prose claim with no check behind it is what
               this whole file is a reaction to.

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
# the 99 it actually scanned).  The 50 that stood here was advertised as "well
# under the real count (the engine was ~190 files)"; the engine has never been
# 190 files, and the 190's provenance is unknown -- the whole repo is **231**
# .cpp/.hpp files (`git ls-files '*.cpp' '*.hpp' | wc -l`, and the same at the
# commit that wrote the 190), and no subtree of this repo comes to 190.  So the
# floor was half the true number under a comment that cited a figure matching
# nothing.  80 is a ~19% margin: enough for ordinary deletion, tight enough
# that a walk finding half the engine fails.
#
# It bounds the WALK and the union separately, in main(), and the distinction
# is the whole value of the number: `sources` is the union of the walk and
# CMake's SOURCES property, so CMake alone supplies 99 and ANY floor below 99
# on the union is clear even when the walk returns nothing.  A floor on the
# union is therefore not a check on the walk -- raising 50 to 80 buys nothing
# by itself, which is why the walk is now floored directly.
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


# A link entry is not always a bare target name, and MATCHING A DECORATED
# ENTRY AS IF IT WERE ONE IS A BYPASS, not a cosmetic problem.  Three forms are
# legal in LINK_LIBRARIES and none of them unwraps to a clean name:
#
#     $<LINK_ONLY:gma_data>                      one genex head
#     $<IF:$<BOOL:ON>,gma_data,>                 commas, nested head
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,gma_data>     CMake >= 3.24, feature + comma
#     $<$<CONFIG:Debug>:gma_data>                head is itself a genex
#     gma::@(0x55f0)                             directory-scope marker on an
#                                                INTERFACE_LINK_LIBRARIES entry
#
# So this file does NOT rely on reducing an entry to one name.  It extracts
# every identifier-shaped TOKEN in the entry and pattern-matches all of them,
# and tries each as a graph key so the walk does not stop an edge short when
# the edge was wrapped.  Over-extraction is the safe direction: a spurious
# token can only cause a false FAILURE, which a human reads, while a missed one
# causes a false pass, which nobody does.
#
# PROVENANCE, because it matters for how much these cases are worth: the
# manifest this gate actually reads contains NONE of them today.
# `$<TARGET_PROPERTY:...>` inside `file(GENERATE)` evaluates the property's own
# generator expressions, and `link.gma_engine` really resolves to
# `Boost::system|Threads::Threads|RapidJSON::rapidjson|OpenSSL::SSL|OpenSSL::Crypto`
# -- no genex, no marker, and `$<LINK_ONLY:` appears nowhere in this repo.
# They are handled because the producer is not the only possible producer of
# this file and because the cost of being wrong is a silent pass; they are NOT
# handled because they are observed.  The selftest cases for them are labelled
# accordingly.
GENEX_HEAD_RE = re.compile(r"^\$<(?:[A-Za-z_0-9]+|\$<[^<>]*>):(.*)>$")
DIRSCOPE_RE = re.compile(r"::@\(?[^)]*\)?$")
# `+`, `.` and `-` appear in real library names (`stdc++`, `libm.so`,
# `foo-bar`); `::` in namespaced targets.  Commas and genex punctuation are
# separators, which is what splits `WHOLE_ARCHIVE,gma_data` apart.
TOKEN_RE = re.compile(r"[A-Za-z0-9_.+:-]+")


def link_item_name(item: str) -> str:
    """The most likely bare target name, for recursion and for the report.

    Best effort only -- `link_item_candidates` is what the checks match on.
    """
    name = item.strip()
    while True:
        m = GENEX_HEAD_RE.match(name)
        if not m:
            break
        name = m.group(1).strip()
    name = DIRSCOPE_RE.sub("", name).strip()
    # A residue like `WHOLE_ARCHIVE,gma_data` or `$<BOOL:ON>,gma_data,` is not
    # a name; prefer its last identifier-shaped token, which is where CMake
    # puts the library in every genex form above.
    if "," in name or "<" in name or ">" in name:
        tokens = [t for t in TOKEN_RE.findall(name) if not t.startswith("$")]
        if tokens:
            name = DIRSCOPE_RE.sub("", tokens[-1])
    return name.strip()


def link_item_candidates(item: str) -> list[str]:
    """Every string in a link entry that could name a library.

    The unwrapped name plus every identifier-shaped token, deduped.  Checks C
    and D match patterns against all of these, so a forbidden name cannot hide
    inside a generator expression this parser does not fully understand.
    """
    out = [link_item_name(item)]
    for tok in TOKEN_RE.findall(item.strip()):
        tok = DIRSCOPE_RE.sub("", tok).strip()
        if tok and tok not in out:
            out.append(tok)
    return [c for c in out if c]


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
                 root: str) -> list[tuple[str, list[str], tuple[str, ...]]]:
    """Every library reachable from `root`: (name, candidates, chain).

    Breadth-first so the reported chain is a shortest one, and cycle-safe.  A
    name with no row in the graph is a leaf (a system library, an imported
    target) and is matched but not expanded.  EVERY candidate of an entry is
    tried as a graph key, not just the best-guess name, so a wrapped edge --
    `$<LINK_LIBRARY:WHOLE_ARCHIVE,gma>` -- is still traversed instead of
    stopping the walk one edge short.
    """
    out: list[tuple[str, list[str], tuple[str, ...]]] = []
    reported: set[str] = {root}
    expanded: set[str] = {root}
    queue: list[tuple[str, tuple[str, ...]]] = [(root, (root,))]
    while queue:
        target, path = queue.pop(0)
        for raw in graph.get(target, ()):
            cands = link_item_candidates(raw)
            if not cands:
                continue
            name = cands[0]
            chain = path + (name,)
            if name not in reported:
                reported.add(name)
                out.append((name, cands, chain))
            for cand in cands:
                if cand in graph and cand not in expanded:
                    expanded.add(cand)
                    queue.append((cand, path + (cand,)))
    return out


# A name that belongs to this repo rather than to the system or a dependency.
# `gma`, `gma_engine`, `gma_data`, `gma::arrow`, `gma::data` all match.
FIRST_PARTY_RE = re.compile(r"^gma(?:[_:].*)?$")


def forbidden_links(graph: dict[str, list[str]], root: str) -> list[str]:
    """Checks C and D: nothing forbidden anywhere in `root`'s link closure.

    Transitive, not direct -- see the docstring.  The chain is printed because
    the fix is almost never on the root's own link line.

    TWO rules, and the second one is what closes the hole ONE LEVEL DEEPER
    than the review found it.  The manifest's target registry is written by
    hand in CMakeLists.txt, and nothing in CMake makes it complete, so a new
    first-party library -- `gma_store`, say -- would arrive with no row, be
    treated as a LEAF, and have its own dependencies never walked; an ALIAS
    (`add_library(gma::data ALIAS gma_data)`) does the same thing without even
    being a new target.  Either one makes GMA_BUILD_DATA=OFF unlinkable while
    rule one stays clean.  So a reached name that LOOKS first-party and has no
    row is itself a failure: not "this links something forbidden" but "this
    subtree was never searched, so a clean verdict about it is a verdict from
    a search that did not happen".
    """
    problems: list[str] = []
    for name, cands, chain in link_closure(graph, root):
        hit = next((c for c in cands
                    for p in FORBIDDEN_LINK_PATTERNS if p.search(c)), None)
        if hit is not None:
            shown = hit if hit == name else f"{name}' (as '{hit}"
            problems.append(
                f"{root} links '{shown}' via {' -> '.join(chain)} -- Arrow, "
                f"the ClickHouse client and gma_data must never be "
                f"reachable from {root}'s link line, because SPEC D14's "
                f"live-only client variant omits gma_data entirely and "
                f"must still link")
            continue
        if FIRST_PARTY_RE.match(name) and name not in graph:
            problems.append(
                f"{root} links '{name}' via {' -> '.join(chain)} -- that is a "
                f"first-party name with NO link.<target> row in the manifest, "
                f"so its own dependencies were never walked and this closure "
                f"is incomplete. Add it to _gma_iso_link_targets in "
                f"CMakeLists.txt (an ALIAS belongs there under its real "
                f"target's name), or if it is genuinely third-party, rename "
                f"it so it does not read as ours")
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
        # The real shape of this repo's link graph, copied from an actual
        # manifest (`link.*` rows of build/gma_engine_isolation.manifest,
        # 2026-10-14) rather than from memory -- OpenSSL included, and with
        # no decoration, because the real rows carry none.
        legal = {
            "gma_server": ["gma", "Boost::system", "Threads::Threads",
                           "RapidJSON::rapidjson"],
            "gma": ["gma_engine", "gma_connector_market"],
            "gma_engine": ["Boost::system", "Threads::Threads",
                           "RapidJSON::rapidjson", "OpenSSL::SSL",
                           "OpenSSL::Crypto"],
            "gma_connector_market": ["gma_engine"],
            "gma_connector_synthetic": ["gma_engine"],
            "gma_tests": ["gma", "Boost::system", "Threads::Threads",
                          "RapidJSON::rapidjson", "GTest::gtest_main",
                          "gma_connector_synthetic"],
        }
        expect_clean("C/closure-legal", forbidden_links(legal, "gma_engine"))
        expect_clean("D/closure-legal", forbidden_links(legal, "gma_server"))
        expect_clean("D/closure-legal-gma_tests",
                     forbidden_links(legal, "gma_tests"))

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

        # The third route, and the one the FIRST VERSION OF THIS CHECK MISSED:
        # the target registry in CMakeLists.txt is hand-written, so a new
        # first-party library gets no row and the walk treats it as a leaf.
        # Both of these are green under a check that only pattern-matches.
        new_lib = dict(legal)
        new_lib["gma"] = ["gma_engine", "gma_connector_market", "gma_store"]
        expect_detected("D/unregistered-first-party-library",
                        forbidden_links(new_lib, "gma_server"))
        aliased = dict(legal)
        aliased["gma"] = ["gma_engine", "gma_connector_market", "gma::data"]
        expect_detected("D/alias-for-gma_data",
                        forbidden_links(aliased, "gma_server"))
        # ... and once the row exists, the walk reaches through it.
        registered = dict(new_lib)
        registered["gma_store"] = ["gma_engine", "gma_data"]
        expect_detected("D/through-a-registered-new-library",
                        forbidden_links(registered, "gma_server"))
        clean_new_lib = dict(new_lib)
        clean_new_lib["gma_store"] = ["gma_engine"]
        expect_clean("D/a-registered-new-library-that-is-clean",
                     forbidden_links(clean_new_lib, "gma_server"))
        # A genuinely third-party leaf must NOT trip the first-party rule.
        expect_clean("D/third-party-leaves-are-leaves",
                     forbidden_links({"gma_server": ["Boost::system", "m",
                                                     "GTest::gtest_main",
                                                     "stdc++fs", "libm.so"]},
                                     "gma_server"))

        # Direct links still caught, one per forbidden pattern.
        for name in ("gma_data", "gma::arrow", "arrow_static", "parquet",
                     "clickhouse-cpp", "duckdb"):
            expect_detected(f"C/direct-{name}",
                            forbidden_links({"gma_engine": ["Threads::Threads",
                                                            name]},
                                            "gma_engine"))

        # DECORATED ENTRIES.  Labelled honestly: the manifest this gate reads
        # carries none of these forms today (see the note above
        # link_item_name), so these are defence against a producer other than
        # the current one, not coverage of an observed case.  They are here
        # because every one of them was a working bypass of the first version
        # of this check.
        for label, entry in (
                ("genex/LINK_ONLY", "$<LINK_ONLY:gma_data>"),
                ("genex/IF", "$<IF:$<BOOL:ON>,gma_data,>"),
                ("genex/LINK_LIBRARY-feature",
                 "$<LINK_LIBRARY:WHOLE_ARCHIVE,gma_data>"),
                ("genex/nested-head", "$<$<CONFIG:Debug>:gma_data>"),
                ("genex/arrow-wrapped", "$<LINK_ONLY:gma::arrow>")):
            expect_detected(f"C/{label} (defensive: not emitted today)",
                            forbidden_links({"gma_engine": [entry]},
                                            "gma_engine"))
        # A wrapped edge must still be TRAVERSED, not merely matched.
        expect_detected("D/wrapped-edge-is-still-walked (defensive)",
                        forbidden_links(
                            {"gma_server": ["$<LINK_LIBRARY:WHOLE_ARCHIVE,gma>"],
                             "gma": ["gma_data"]},
                            "gma_server"))
        expect_detected("C/directory-scoped-interface-entry (defensive)",
                        forbidden_links(
                            {"gma_server": ["gma::@(0x55f0)"],
                             "gma": ["gma_data"]},
                            "gma_server"))
        # And a decorated LEGAL entry must not be mistaken for a violation.
        expect_clean("C/decorated-legal-entry (defensive)",
                     forbidden_links(
                         {"gma_engine": ["$<LINK_ONLY:Boost::headers>",
                                         "$<LINK_LIBRARY:WHOLE_ARCHIVE,m>"]},
                         "gma_engine"))

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

        # cmake_true, because the positive control's verdict depends on it.
        for truthy in ("ON", "on", "1", "TRUE", "yes", "Y", "2"):
            if not cmake_true(truthy):
                failures.append(f"SELFTEST: cmake_true({truthy!r}) is False")
        for falsy in ("OFF", "off", "0", "FALSE", "no", "", "NOTFOUND", "junk"):
            if cmake_true(falsy):
                failures.append(f"SELFTEST: cmake_true({falsy!r}) is True")

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
    tests_closure = (link_closure(graph, "gma_tests")
                     if "gma_tests" in graph else [])
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
    # Both counts, separately, and that is the point. `sources` is the UNION
    # of the walk and CMake's own source list, so CMake alone supplies 99 and a
    # floor on the union cannot detect a walk that returned nothing -- which is
    # the failure the floor is named for. The walk gets its own floor.
    if len(walked) < MIN_ENGINE_FILES:
        vacuous.append(
            f"the filesystem walk of src/ and include/ found {len(walked)} "
            f"source/header files (floor {MIN_ENGINE_FILES}) -- the traversal "
            f"is blind, and because `sources` is the union of the walk and "
            f"CMake's list, a clean check A result would otherwise look "
            f"perfectly normal")
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
    if not any(name == "gma_engine" for name, _, _ in server_closure):
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
                forbidden_links(graph, "gma_server") +
                (forbidden_links(graph, "gma_tests")
                 if "gma_tests" in graph else []))

    print(f"gma_engine isolation (ENC-1481, SPEC D14)")
    print(f"  root            : {root}")
    print(f"  engine files    : {len(sources)} "
          f"({len(cmake_sources)} from CMake, {len(walked)} walked)")
    print(f"  include dirs    : {len(include_dirs)}")
    print(f"  link libraries  : {len(link_libs)} engine, {len(server_libs)} server "
          f"(direct)")
    print(f"  link closure    : {len(engine_closure)} from gma_engine, "
          f"{len(server_closure)} from gma_server, "
          f"{len(tests_closure)} from gma_tests, over "
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
          "in the TRANSITIVE link closure of gma_engine, gma_server or "
          "gma_tests.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
