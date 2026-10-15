//
// gma_ingest — the ENC-1483 ingest job's command line.
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D5, D6, D7, D11;
//         docs/data-layer.md §8.6, §8.7, §8.8, §8.9.
//
//   gma_ingest ingest  [--url U] [--data-root P] [--session-id N]
//                      [--dbn-version V] [--no-insert] [--force-reencode]
//   gma_ingest reload  [--url U] [--data-root P]      # ingest --from-dbn-only
//   gma_ingest verify  [--url U] [--symbol S]         # the D11 demonstration
//   gma_ingest resolve-target [--url U]               # which server, no I/O
//
// EXIT CODES, and the distinction is the point: 0 clean, 1 A FINDING (the data
// or the completeness rows are not what they should be), 2 COULD NOT RUN (no
// server, no sample, a missing metadata.json). A tool that returns 1 for both
// makes "the gate is red" and "the gate never ran" the same observation.
//

#include "gma/data/IngestJob.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int kClean       = 0;
constexpr int kFinding     = 1;
constexpr int kCouldNotRun = 2;

// The sample's default location. OUTSIDE every git working tree on purpose —
// licensed vendor data, and docs/data-layer.md §14.2 names committing Databento
// data a redistribution question not worth answering. Overridable because a
// default is not a hardcoding.
constexpr const char* kDefaultDataRoot =
    "~/market-data/databento/XNAS.ITCH/MSFT/20220610";

constexpr std::int64_t kDayNs = 86400000000000LL;

void usage() {
  std::cout <<
      "usage: gma_ingest <ingest|reload|verify|resolve-target> [options]\n"
      "\n"
      "  ingest           CSV -> DBN (retained) -> adapter -> gma.\n"
      "  reload           the SPEC D6 reload: the retained DBN only, no CSV.\n"
      "  verify           the D11 demonstration, through the DDL's read rule.\n"
      "  resolve-target   print which ClickHouse would be used; touch nothing.\n"
      "\n"
      "  --url <u>          ClickHouse HTTP base URL\n"
      "  --data-root <p>    where the vendor files live (default\n"
      "                     " << kDefaultDataRoot << ")\n"
      "  --symbol <s>       verify: the ticker (default MSFT)\n"
      "  --session-id <n>   override the derived session_id\n"
      "  --dbn-version <v>  DBN version to emit (1..3, default 3)\n"
      "  --no-insert        ingest: do the raw half only, touch no table\n"
      "  --force-reencode   ingest: rewrite the retained DBN even if identical\n"
      "\n"
      "exit: 0 clean, 1 a finding, 2 could not run\n";
}

struct Args {
  std::string command;
  std::optional<std::string> url;
  std::string dataRoot = kDefaultDataRoot;
  std::string symbol   = "MSFT";
  std::uint32_t sessionId = 0;
  std::uint8_t dbnVersion = 3;
  bool noInsert = false;
  bool forceReencode = false;
};

// An unknown flag is FATAL rather than collected and dropped. The same defect
// in tools/linear.mjs filed twelve Linear tickets with empty bodies and exited
// 0 each time (CLAUDE.md, Work guardrails); a tool that accepts
// `--session_id 7` and silently ingests under a derived session is the same
// shape of lie.
bool parseArgs(int argc, char** argv, Args& a) {
  if (argc < 2) return false;
  a.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string f = argv[i];
    const auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "gma_ingest: " << name << " needs a value\n";
        std::exit(kCouldNotRun);
      }
      return argv[++i];
    };
    // ⚠ CODE-REVIEW FINDINGS #2 AND #4, which are one bug with two volumes.
    //
    // Both numeric flags went through an unchecked `strtoul`, which returns 0
    // on anything unparseable and reports nothing:
    //
    //   #2 LOUD: `--dbn-version notanumber` became version 0, which
    //      `DbnWriter::build()` rejects by THROWING `std::logic_error` out of
    //      the `arrow::Result` path with nothing to catch it. Reproduced:
    //      exit 134. **A crash is not one of this tool's three documented exit
    //      codes**, and an abort skips every refusal path including the
    //      `state=2` write.
    //   #4 QUIET, AND WORSE: `--session-id notanumber` became 0, which this
    //      tool defines as "derive it", so the run proceeded under the derived
    //      FNV session, exited 0, and said nothing. The comment directly above
    //      this function condemns precisely that shape — "a tool that accepts
    //      `--session_id 7` and silently ingests under a derived session is the
    //      same shape of lie".
    //
    // So both parse exactly or exit 2. `--dbn-version` is additionally bounded
    // by the versions the DECODER accepts, because emitting a version the
    // adapter refuses is how finding #3 destroyed a retained D6 baseline.
    const auto num = [&](const char* name, auto lo, auto hi) -> std::uint64_t {
      const char* text = next(name);
      std::uint64_t v = 0;
      const char* last = text + std::strlen(text);
      const auto r = std::from_chars(text, last, v);
      if (r.ec != std::errc{} || r.ptr != last) {
        std::cerr << "gma_ingest: " << name << " must be an integer (got \""
                  << text << "\")\n";
        std::exit(kCouldNotRun);
      }
      if (v < static_cast<std::uint64_t>(lo) || v > static_cast<std::uint64_t>(hi)) {
        std::cerr << "gma_ingest: " << name << " must be in [" << +lo << ", " << +hi
                  << "] (got " << v << ")\n";
        std::exit(kCouldNotRun);
      }
      return v;
    };
    if (f == "--url") {
      a.url = next("--url");
    } else if (f == "--data-root") {
      a.dataRoot = next("--data-root");
    } else if (f == "--symbol") {
      a.symbol = next("--symbol");
    } else if (f == "--session-id") {
      // 1.. : 0 is reserved as "derive from the retained file", and accepting
      // it here would make an explicit --session-id 0 indistinguishable from
      // not passing the flag — the #4 ambiguity, re-created deliberately.
      a.sessionId = static_cast<std::uint32_t>(
          num("--session-id", 1u, std::numeric_limits<std::uint32_t>::max()));
    } else if (f == "--dbn-version") {
      a.dbnVersion = static_cast<std::uint8_t>(
          num("--dbn-version", gma::data::dbn::kMinVersion, gma::data::dbn::kMaxVersion));
    } else if (f == "--no-insert") {
      a.noInsert = true;
    } else if (f == "--force-reencode") {
      a.forceReencode = true;
    } else if (f == "-h" || f == "--help") {
      usage();
      std::exit(kClean);
    } else {
      std::cerr << "gma_ingest: unknown flag " << f << "\n";
      return false;
    }
  }
  return true;
}

using namespace gma::data;

// Guarded integer parse for server text (finding #8). `std::stoll` throws, and
// nothing on this path is prepared for an exception.
bool parseLL(const std::string& text, long long& out) {
  if (text.empty()) return false;
  const char* last = text.data() + text.size();
  const auto r = std::from_chars(text.data(), last, out);
  return r.ec == std::errc{} && r.ptr == last;
}

// AN INDEPENDENT ANSWER to "is [from, to) inside the union of `held`?", written
// for finding #14 so the probes compare two implementations instead of pinning
// a constant.
//
// Deliberately a DIFFERENT ALGORITHM from `gma::data::isComplete`'s cursor
// walk: it subtracts each held interval from the request and asks whether
// anything is left. Same answer by a different route, which is the only version
// of this that is worth running — a copy of the cursor walk would agree with a
// bug in the cursor walk.
bool expectedCovered(const std::vector<Interval>& held, std::int64_t from,
                     std::int64_t to) {
  if (to <= from) return false;  // matches isComplete: not trivially complete
  std::vector<Interval> remaining{Interval{from, to}};
  for (const auto& h : held) {
    std::vector<Interval> next;
    for (const auto& r : remaining) {
      if (h.toNs <= r.fromNs || h.fromNs >= r.toNs) {
        next.push_back(r);  // disjoint
        continue;
      }
      if (h.fromNs > r.fromNs) next.push_back(Interval{r.fromNs, h.fromNs});
      if (h.toNs < r.toNs) next.push_back(Interval{h.toNs, r.toNs});
    }
    remaining = std::move(next);
    if (remaining.empty()) return true;
  }
  return remaining.empty();
}

void printTarget(const ch::Target& t) {
  std::cout << "clickhouse : " << t.url << "   (" << t.how << ")\n";
  if (t.offsetWasSet) {
    std::cout << "             STACK_PORT_OFFSET=" << t.stackOffset
              << (t.explicitUrl
                      ? " is set but an explicit target wins\n"
                      : " -> this is THIS worktree's server, not offset zero\n");
  }
}

void printKind(const ingest::KindReport& k) {
  std::cout << "\n--- " << k.kind << " (vendor schema `" << k.dbnSchema << "`) ---\n";
  if (!k.csvPath.empty()) std::cout << "  csv            : " << k.csvPath << "\n";
  std::cout << "  retained dbn   : " << k.dbnPath << "  (" << k.dbnBytes
            << " bytes, " << (k.dbnRewritten ? "WRITTEN" : (k.dbnExisted ? "unchanged" : "written"))
            << ")\n";
  if (!k.jobId.empty()) std::cout << "  vendor job_id  : " << k.jobId << "\n";
  std::cout << "  symbol         : " << k.symbol << "  instrument_id "
            << k.instrumentId << "  venue " << k.venue << "\n";
  std::cout << "  session_id     : " << k.sessionId << "\n";
  if (k.csvRecords != 0) std::cout << "  csv records    : " << k.csvRecords << "\n";
  std::cout << "  dbn records    : " << k.decodedRecords << "\n";
  std::cout << "  canonical rows : " << k.canonicalRows << "\n";
  std::cout << "  rows inserted  : " << k.rowsInserted << "\n";
  if (!k.actionCounts.empty()) {
    std::cout << "  action mix     :";
    for (const auto& [a, n] : k.actionCounts) std::cout << ' ' << a << '=' << n;
    // The replace and clear paths are NOT exercised by this data. Said plainly
    // rather than left to be inferred from an absence.
    bool hasM = false;
    bool hasR = false;
    for (const auto& [a, n] : k.actionCounts) {
      hasM = hasM || a == 'M';
      hasR = hasR || a == 'R';
    }
    std::cout << "   (M " << (hasM ? "present" : "ABSENT") << ", R "
              << (hasR ? "present" : "ABSENT")
              << " -> the replace and clear/epoch-bump paths are NOT exercised "
                 "by this data)\n";
  }
  if (k.csvRecords != 0) {
    std::cout << "  ts_recv == ts_event + ts_in_delta : " << k.tsRecvMatchesDelta
              << " / " << k.csvRecords << "\n";
  }
  std::cout << "  DBN side 'N' on a row (kGmaFlagSideUnspecified) : "
            << k.sideUnspecified << "\n";
  std::cout << "  sort key (symbol, ts_event, provider_seq): "
            << k.distinctSortKeys << " distinct over " << k.canonicalRows
            << " rows, " << k.duplicateSortKeys
            << " row(s) share a key with another"
            << (k.duplicateSortKeys != 0
                    ? "  <- NOT UNIQUE; D5's plain MergeTree is load-bearing"
                    : "")
            << "\n";
  std::cout << "  pull limit     : " << k.pullLimit
            << (k.truncatedByLimit ? "   TRUNCATED (records == limit)" : "")
            << "\n";
  std::cout << "  requested      : "
            << ingest::formatWindow(k.requestedFromNs, k.requestedToNs) << "\n";
  std::cout << "  RECORDED       : "
            << ingest::formatWindow(k.recordedFromNs, k.recordedToNs) << "\n";
  if (k.truncatedByLimit) {
    std::cout
        << "    ^ the pull returned exactly its `limit`, so the delivered data "
           "is a PREFIX of\n"
           "      the requested window and the recorded `to_ts` is the last "
           "ts_event, EXCLUSIVE.\n"
           "      "
        << k.recordsAtLastTsEvent
        << " record(s) carry that instant and nothing in the file says whether "
           "the cut\n"
           "      fell inside the group, so the possibly-partial final instant "
           "is dropped from\n"
           "      the CLAIM (the rows themselves are inserted). Recording the "
           "requested window\n"
           "      instead would be the docs §8.8 failure in the one table that "
           "exists to prevent it.\n";
  }
  std::cout << "  round trip     : " << (k.roundTripOk ? "OK" : "FAILED") << " — "
            << k.roundTripNote << "\n";
}

int cmdIngest(const Args& a, bool fromDbnOnly) {
  ingest::Options o;
  o.dataRoot      = a.dataRoot;
  o.url           = a.url;
  o.sessionId     = a.sessionId;
  o.dbnVersion    = a.dbnVersion;
  o.fromDbnOnly   = fromDbnOnly;
  o.noInsert      = a.noInsert;
  o.forceReencode = a.forceReencode;

  auto res = ingest::run(o);
  if (!res.ok()) {
    // ⚠ CODE-REVIEW FINDING #1. This was an unconditional `return
    // kCouldNotRun`, so every one of the job's data refusals — a round-trip
    // mismatch, a retained DBN that would not read back, two tickers for one
    // `instrument_id`, an empty delivered window — reported as "the gate never
    // ran". That is the conflation this file's own header warns about, inverted,
    // and it is the worse direction: a sweep that reads 2 as "environment,
    // retry later" throws the finding away. The classification is made at each
    // refusal site and carried on the status; see Verdict.hpp.
    const int code = exitCodeFor(res.status());
    std::cerr << "gma_ingest: " << res.status().ToString() << "\n"
              << "gma_ingest: exiting " << code << " — "
              << (code == kFinding
                      ? "A FINDING: the job ran and what it found is wrong."
                      : "COULD NOT RUN: no verdict was reached.")
              << "\n";
    return code;
  }
  const auto& rep = *res;
  printTarget(rep.target);
  std::cout << "session_id : FNV-1a-32 of each retained .dbn — see the ruling "
               "below\n";
  for (const auto& k : rep.kinds) printKind(k);

  std::cout << "\nsymbology rows written : " << rep.symbologyRows << "\n";
  std::cout << "partitions dropped     :";
  for (const auto& p : rep.partitionsDropped) std::cout << ' ' << p;
  std::cout << "\n";
  std::cout << "invalidation rows      : " << rep.invalidationRows.size()
            << " (other symbols whose completeness the DROP PARTITION voided)\n";
  for (const auto& r : rep.invalidationRows) std::cout << "    " << r << "\n";

  std::cout << "\n=== session_id reuse — THE RULING (assigned here from ENC-1480) ===\n"
            << rep.sessionIdRuling << "\n";
  std::cout << "\n=== gma.gaps ===\n" << rep.gapsNote << "\n";
  return kClean;
}

// ---------------------------------------------------------------------------
// verify — the D11 demonstration.
//
// Every verdict goes through gma::data::isComplete over rows selected from
// gma.ingest_windows, i.e. through the DDL's own read rule: newest row by
// updated_at per recorded window, require state = 1, then test COVERAGE of the
// request. Row counts are printed and NEVER consulted — a quiet window and a
// missing window have the same count (D11), which is the entire reason the
// table exists.
// ---------------------------------------------------------------------------
int cmdVerify(const Args& a) {
  auto target = ch::resolveTarget(a.url);
  if (!target.ok()) {
    std::cerr << "gma_ingest: " << target.status().ToString() << "\n";
    return kCouldNotRun;
  }
  const ch::Client client(*target);
  printTarget(*target);

  int findings = 0;
  const struct {
    const char* kind;
    const char* table;
  } kinds[] = {{"trades", "trades"}, {"book_events", "book_events"}};

  for (const auto& k : kinds) {
    std::cout << "\n================ " << a.symbol << " / " << k.kind
              << " ================\n";

    // Learn what is recorded by asking the rule about a request so wide that
    // its `covered` list IS the held set. Note the deliberate circularity-
    // avoidance: the windows are not read from a previous run's output, they
    // are read from the server through the same rule a query would use.
    const std::int64_t probeFrom = 0;
    const std::int64_t probeTo   = 4000000000000000000LL;  // ~2096
    auto wide = ingest::queryCompleteness(client, a.symbol, k.kind, probeFrom, probeTo);
    if (!wide.ok()) {
      std::cerr << "gma_ingest: " << wide.status().ToString() << "\n";
      return kCouldNotRun;
    }
    std::cout << "recorded windows resolved : " << wide->resolved
              << ", surviving state=1 after resolution : " << wide->survived << "\n";
    if (wide->covered.empty()) {
      std::cout << "  NOTHING is recorded as complete for this (symbol, kind). "
                   "Run `gma_ingest ingest` first.\n";
      ++findings;
      continue;
    }
    for (const auto& c : wide->covered) {
      std::cout << "  held : " << ingest::formatWindow(c.fromNs, c.toNs) << "  ("
                << (c.toNs - c.fromNs) / 1000000000.0 << " s)\n";
    }
    const auto held = wide->covered.front();
    const std::int64_t dayStart =
        static_cast<std::int64_t>(csv::midnightUtcNs(
            csv::yyyymmddUtc(static_cast<std::uint64_t>(held.fromNs))));

    struct Probe {
      const char*  name;
      std::int64_t from;
      std::int64_t to;
    };
    const Probe probes[] = {
        // The request the ticket asks about: the full UTC trading day.
        {"the FULL trading day", dayStart, dayStart + kDayNs},
        // And the sharper one: the window the pull itself REQUESTED.
        {"the window the pull REQUESTED [12:30, 14:00)",
         dayStart + 45000000000000LL, dayStart + 50400000000000LL},
        // Exactly what is held — the boundary must be exact in both directions.
        {"exactly the recorded window", held.fromNs, held.toNs},
        // Strictly inside.
        {"strictly inside the recorded window", held.fromNs + 1,
         held.toNs - 1 > held.fromNs + 1 ? held.toNs - 1 : held.toNs},
        // One nanosecond past the end: the smallest possible overrun.
        {"one nanosecond past the recorded end", held.toNs - 1, held.toNs + 1},
        // One nanosecond before the start.
        {"one nanosecond before the recorded start", held.fromNs - 1,
         held.fromNs + 1},
    };

    for (const auto& p : probes) {
      auto v = ingest::queryCompleteness(client, a.symbol, k.kind, p.from, p.to);
      if (!v.ok()) {
        std::cerr << "gma_ingest: " << v.status().ToString() << "\n";
        return kCouldNotRun;
      }
      // ⚠ CODE-REVIEW FINDING #14: THE EXPECTATION IS DERIVED, NOT PINNED.
      //
      // It used to be a hardcoded `expectComplete=false` for the two headline
      // probes, which encoded a property of THIS TRUNCATED SAMPLE as a property
      // of the system: once a full day is actually ingested — the intended end
      // state — `verify` would have reported the full-day probe as [FAIL] and
      // exited 1. **A gate that fails when the system improves is not a gate.**
      //
      // `expectedCovered` computes the answer from the held set by an
      // independently written algorithm (subtract-and-check-empty rather than
      // the library's cursor walk), so each probe asserts that TWO
      // implementations of "is this request inside the union of held windows"
      // agree — which is a real cross-check and adapts by construction.
      const bool expected = expectedCovered(wide->covered, p.from, p.to);
      const bool pass = v->complete == expected;
      std::cout << (pass ? "  [ok]   " : "  [FAIL] ") << p.name << "\n"
                << "         request " << ingest::formatWindow(p.from, p.to) << "\n"
                << "         isComplete -> " << (v->complete ? "COMPLETE" : "REFUSED")
                << ", independently derived expectation "
                << (expected ? "COMPLETE" : "REFUSED");
      if (v->gap.has_value()) {
        std::cout << ", first uncovered "
                  << ingest::formatWindow(v->gap->fromNs, v->gap->toNs);
      }
      std::cout << "\n";
      if (!pass) ++findings;

      // Row counts, printed ALONGSIDE the verdict and never instead of it.
      auto n = ingest::rowCount(client, k.table, a.symbol, p.from, p.to);
      if (!n.ok()) {
        std::cerr << "gma_ingest: " << n.status().ToString() << "\n";
        return kCouldNotRun;
      }
      std::cout << "         gma." << k.table << " rows in that range: " << *n
                << "   <- DIAGNOSTIC ONLY; a quiet window and a missing window "
                   "have the same count (D11)\n";
    }

    // ---------------------------------------------------------------------
    // A COUNT CANNOT ESTABLISH COMPLETENESS. IT CAN REFUTE A CLAIM.
    //
    // D11 forbids inferring completeness from row counts, and this does not do
    // that — every verdict above came from the read rule. But the implication
    // runs one way: a window whose newest row says `state = 1` with
    // `rows_inserted = N > 0` and which now holds ZERO rows is not "quiet", it
    // is a LIE, and the arithmetic is sound in that direction.
    //
    // This exists because the hazard was MEASURED. The DDL's invariant block
    // makes maintaining the table a code obligation of the ingest job: when the
    // job drops a partition it writes a `state=3`/`state=0` row first. A
    // `DROP PARTITION` typed at the server by hand bypasses that entirely — and
    // when it was, `isComplete` for the in-window request returned COMPLETE
    // over an empty table, with every probe above still [ok] because the rule
    // was behaving correctly over a table that had been lied to. There is no
    // foreign key and no trigger that can prevent it, so the next best thing is
    // for the verifier to notice.
    //
    // ⚠ CODE-REVIEW FINDING #7: IT IS SCOPED TO ONE RECORDED WINDOW NOW. The
    // first version compared a window-INDEPENDENT `max(rows_inserted)` over the
    // whole (symbol, kind) against the count inside one MERGED interval,
    // ignoring both which window it was looking at and the newest-row
    // resolution. With two disjoint covered intervals that prints a [FAIL]
    // about a window that claimed nothing. So it iterates
    // `survivingWindows` — the UNMERGED windows that the read rule already
    // resolved to `state = 1` — and asks the table about exactly that
    // `(from_ts, to_ts)`.
    for (const auto& w : wide->survivingWindows) {
      std::ostringstream q;
      q << "SELECT rows_inserted, toUnixTimestamp64Milli(updated_at) "
           "FROM gma.ingest_windows WHERE symbol = "
        << ch::sqlQuote(a.symbol) << " AND kind = " << ch::sqlQuote(k.kind)
        << " AND from_ts = fromUnixTimestamp64Nano(toInt64(" << w.fromNs
        << "), 'UTC') AND to_ts = fromUnixTimestamp64Nano(toInt64(" << w.toNs
        << "), 'UTC') AND state = 1 FORMAT TabSeparated";
      auto rows = client.select(q.str());
      if (!rows.ok()) {
        std::cerr << "gma_ingest: " << rows.status().ToString() << "\n";
        return kCouldNotRun;
      }
      // Newest state=1 row for THIS window, resolved in C++ like everything
      // else on this path. Guarded parsing (finding #8): `\N` or an empty field
      // from the server must not throw out of a function that reports status.
      long long claimedN = 0;
      long long newest = -1;
      for (const auto& r : *rows) {
        if (r.size() != 2) {
          std::cerr << "gma_ingest: gma.ingest_windows row has " << r.size()
                    << " field(s), expected 2\n";
          return kCouldNotRun;
        }
        long long inserted = 0;
        long long updated = 0;
        if (!parseLL(r[0], inserted) || !parseLL(r[1], updated)) {
          std::cerr << "gma_ingest: ClickHouse returned a non-integer for "
                       "rows_inserted/updated_at (\""
                    << r[0] << "\", \"" << r[1] << "\")\n";
          return kCouldNotRun;
        }
        if (updated > newest) {
          newest = updated;
          claimedN = inserted;
        }
      }
      auto actual = ingest::rowCount(client, k.table, a.symbol, w.fromNs, w.toNs);
      if (!actual.ok()) {
        std::cerr << "gma_ingest: " << actual.status().ToString() << "\n";
        return kCouldNotRun;
      }
      if (claimedN > 0 && *actual == 0) {
        std::cout << "  [FAIL] the state=1 window "
                  << ingest::formatWindow(w.fromNs, w.toNs) << " claims "
                  << claimedN << " inserted row(s) and holds ZERO.\n"
                     "         The read rule is behaving correctly over a table "
                     "that has been lied to — the usual\n"
                     "         cause is a DROP PARTITION that did not go through "
                     "the ingest job (it writes a\n"
                     "         state=3 or state=0 row first; a hand-typed ALTER "
                     "does not). Run `gma_ingest reload`.\n";
        ++findings;
      } else {
        std::cout << "  [ok]   the state=1 claim on "
                  << ingest::formatWindow(w.fromNs, w.toNs)
                  << " is not refuted: " << *actual << " row(s) present, "
                  << claimedN << " claimed inserted\n";
      }
    }
  }

  // Symbology, which is the other half of D7.
  std::cout << "\n================ gma.symbology ================\n";
  // No interpolation here at all — the whole statement is a literal.
  auto sym = client.select(
      "SELECT dataset, instrument_id, raw_symbol, toString(valid_from), "
      "toString(valid_to), source_ref FROM gma.symbology FINAL ORDER BY "
      "dataset, instrument_id, valid_from FORMAT TabSeparated");
  if (!sym.ok()) {
    std::cerr << "gma_ingest: " << sym.status().ToString() << "\n";
    return kCouldNotRun;
  }
  if (sym->empty()) {
    std::cout << "  NO ROWS. SPEC D7 requires the mapping, and it is the one "
                 "table that cannot be retrofitted.\n";
    ++findings;
  }
  for (const auto& r : *sym) {
    for (std::size_t i = 0; i < r.size(); ++i) {
      std::cout << (i == 0 ? "  " : "  |  ") << r[i];
    }
    std::cout << "\n";
  }

  std::cout << "\n" << (findings == 0 ? "VERDICT: clean" : "VERDICT: FINDINGS")
            << " (" << findings << ")\n";
  return findings == 0 ? kClean : kFinding;
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parseArgs(argc, argv, a)) {
    usage();
    return kCouldNotRun;
  }
  if (a.command == "ingest") return cmdIngest(a, /*fromDbnOnly=*/false);
  if (a.command == "reload") return cmdIngest(a, /*fromDbnOnly=*/true);
  if (a.command == "verify") return cmdVerify(a);
  if (a.command == "resolve-target") {
    auto t = gma::data::ch::resolveTarget(a.url);
    if (!t.ok()) {
      std::cerr << "gma_ingest: " << t.status().ToString() << "\n";
      return kCouldNotRun;
    }
    printTarget(*t);
    return kClean;
  }
  std::cerr << "gma_ingest: unknown command " << a.command << "\n";
  usage();
  return kCouldNotRun;
}
