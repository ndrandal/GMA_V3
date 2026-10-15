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

#include <cstdlib>
#include <cstring>
#include <iostream>
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
    if (f == "--url") {
      a.url = next("--url");
    } else if (f == "--data-root") {
      a.dataRoot = next("--data-root");
    } else if (f == "--symbol") {
      a.symbol = next("--symbol");
    } else if (f == "--session-id") {
      a.sessionId = static_cast<std::uint32_t>(std::strtoul(next("--session-id"), nullptr, 10));
    } else if (f == "--dbn-version") {
      a.dbnVersion = static_cast<std::uint8_t>(std::strtoul(next("--dbn-version"), nullptr, 10));
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
    std::cerr << "gma_ingest: " << res.status().ToString() << "\n";
    return kCouldNotRun;
  }
  const auto& rep = *res;
  printTarget(rep.target);
  std::cout << "session_id : derived PER KIND from (dataset | schema | job_id) — "
               "see the ruling below\n";
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
      bool expectComplete;
    };
    const Probe probes[] = {
        // THE REFUSAL THE TICKET ASKS FOR: the full UTC trading day.
        {"the FULL trading day", dayStart, dayStart + kDayNs, false},
        // And the sharper one: the window the pull itself REQUESTED. Even that
        // is not covered, because `limit: 1000` truncated the delivery.
        {"the REQUESTED 90-minute window [12:30, 14:00)",
         dayStart + 45000000000000LL, dayStart + 50400000000000LL, false},
        // Exactly what is held — the boundary must be exact in both directions.
        {"exactly the recorded window", held.fromNs, held.toNs, true},
        // Strictly inside.
        {"strictly inside the recorded window", held.fromNs + 1,
         held.toNs - 1 > held.fromNs + 1 ? held.toNs - 1 : held.toNs, true},
        // One nanosecond past the end: the smallest possible overrun must fail.
        {"one nanosecond past the recorded end", held.toNs - 1, held.toNs + 1, false},
        // One nanosecond before the start.
        {"one nanosecond before the recorded start", held.fromNs - 1, held.fromNs + 1,
         false},
    };

    for (const auto& p : probes) {
      auto v = ingest::queryCompleteness(client, a.symbol, k.kind, p.from, p.to);
      if (!v.ok()) {
        std::cerr << "gma_ingest: " << v.status().ToString() << "\n";
        return kCouldNotRun;
      }
      const bool pass = v->complete == p.expectComplete;
      std::cout << (pass ? "  [ok]   " : "  [FAIL] ") << p.name << "\n"
                << "         request " << ingest::formatWindow(p.from, p.to) << "\n"
                << "         isComplete -> " << (v->complete ? "COMPLETE" : "REFUSED")
                << " (expected " << (p.expectComplete ? "COMPLETE" : "REFUSED") << ")";
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
  }

  // Symbology, which is the other half of D7.
  std::cout << "\n================ gma.symbology ================\n";
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
