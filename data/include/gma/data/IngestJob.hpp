#pragma once
//
// The ingest job: retained Databento raw -> canonical records -> L1 (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D5, D6, D7, D8, D11, D12;
//         docs/data-layer.md §8.4, §8.6, §8.7, §8.8, §8.9.
//
// THE PATH, AND EVERY LEG OF IT IS THE REAL ONE
// ---------------------------------------------
//
//     vendor CSV ──DatabentoCsv──▶ dbn::MboMsg / dbn::TradeMsg
//                 ──DbnEncoder───▶ REAL DBN BYTES, written to disk (D6)
//                 ──read back────▶ the retained file, from disk
//                 ──DatabentoAdapter──▶ TickRecord / BookRecord
//                 ──TabSeparated──▶ gma.trades / gma.book_events
//
// The read-back is not ceremony. D5's re-ingest is `DROP PARTITION` + reload
// and D6 makes the retained files what it reloads FROM, so the job decodes the
// file on disk on every run — the reload path is the ONLY insert path, and it
// is therefore exercised on the first ingest rather than first used in anger
// during a recovery. `Options::fromDbnOnly` drops the CSV legs entirely and is
// the pure reload: the DBN metadata carries `start`, `end` and `limit`, so a
// reload needs nothing but the file.
//
// WHAT IT DOES NOT DO, deliberately (SPEC §6 assigns each elsewhere): no shim
// planner, no SQL lint, no event time, no `ReplaySource`, no sink, no
// benchmark. And no B1/B2 number — at 1000 records fixed costs dominate and a
// rows/sec from this would be a wrong number, not a weak one (ENC-1485/1487).
//
// `book_snapshots` IS NOT WRITTEN, and that is a scope statement rather than
// an omission. A snapshot is reconstructed book STATE (docs §8.5) and
// reconstructing it is `OrderBookManager`'s job — the single implementation —
// which this job does not drive. With a 68-second MBO window there is also no
// session open to snapshot at.
//
// `gaps` IS NOT WRITTEN EITHER, and the reason is a measurement rather than
// scope: see `Report::gapsNote`.
//

#include "gma/data/ClickHouseHttp.hpp"
#include "gma/data/DatabentoCsv.hpp"
#include "gma/data/IngestWindows.hpp"

#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gma::data::ingest {

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct Options {
  // WHERE THE SAMPLE LIVES, AND WHY IT IS A PARAMETER WITH THIS DEFAULT.
  // `~/market-data/databento/...` is OUTSIDE every git working tree on
  // purpose: it is licensed vendor data and docs/data-layer.md §14.2 names
  // committing Databento data a redistribution question not worth answering.
  // So the path is configurable, nothing here embeds a row of it, and the
  // retained DBN (D6) is written beside the CSVs rather than into the repo.
  std::string dataRoot;

  std::optional<std::string> url;  // ClickHouse; see ch::resolveTarget

  // 0 means DERIVE it from the pull — see `Report::sessionIdRuling`.
  std::uint32_t sessionId = 0;

  std::uint8_t dbnVersion = 3;

  // Skip the CSV legs and decode the retained DBN only. The D6 reload.
  bool fromDbnOnly = false;

  // Transcode, retain and round-trip, but touch no table. Also the way to run
  // the whole raw half with no server.
  bool noInsert = false;

  // Re-encode the DBN even if a retained file is already byte-identical.
  // Normally a no-op; the flag exists so a deliberate re-encode after an
  // encoder change is not indistinguishable from the idempotent case.
  bool forceReencode = false;
};

// ---------------------------------------------------------------------------
// What one kind's ingest produced.
// ---------------------------------------------------------------------------
struct KindReport {
  std::string kind;        // 'trades' | 'book_events' — the ingest_windows value
  std::string dbnSchema;   // 'trades' | 'mbo'          — the vendor's name
  std::string csvPath;
  std::string dbnPath;
  std::string jobId;       // vendor batch id -> ingest_windows.source_ref
  std::string symbol;
  std::uint32_t instrumentId = 0;
  std::string venue;
  // PER KIND, not per run: it is FNV-1a-32(dataset | schema | job_id), and the
  // two kinds are two vendor jobs. See `Report::sessionIdRuling`.
  std::uint32_t sessionId = 0;

  std::size_t csvRecords  = 0;
  std::size_t dbnBytes    = 0;
  bool        dbnExisted  = false;
  bool        dbnRewritten = false;

  std::size_t decodedRecords = 0;  // DBN records framed by the adapter
  std::size_t canonicalRows  = 0;  // TickRecords or BookRecords produced
  std::size_t rowsInserted   = 0;

  // The REQUESTED window from the pull metadata, and the window actually
  // RECORDED in gma.ingest_windows. They differ when the pull was truncated,
  // and that difference is the whole D11 story on this sample.
  std::int64_t requestedFromNs = 0;
  std::int64_t requestedToNs   = 0;
  std::int64_t recordedFromNs  = 0;
  std::int64_t recordedToNs    = 0;
  std::uint64_t pullLimit      = 0;
  bool truncatedByLimit        = false;
  std::size_t recordsAtLastTsEvent = 0;

  // Round trip (acceptance criterion 2).
  bool roundTripOk = false;
  std::string roundTripNote;

  // Confirmations of the facts the ticket says to confirm rather than
  // rediscover. Reported as counts so "91 of 91" is visible, not implied.
  std::size_t tsRecvMatchesDelta = 0;
  std::vector<std::pair<char, std::size_t>> actionCounts;
  std::size_t sideUnspecified = 0;

  // Free measurement for ENC-1484, since the rows are in hand: how many
  // (symbol, ts_event, provider_seq) triples are shared by more than one row.
  // D5 chose plain MergeTree precisely because the sort key is not unique;
  // this is the number.
  std::size_t duplicateSortKeys = 0;
  std::size_t distinctSortKeys  = 0;
};

struct Report {
  ch::Target target;
  std::vector<KindReport> kinds;
  std::size_t symbologyRows = 0;
  std::vector<std::string> partitionsDropped;
  std::vector<std::string> invalidationRows;  // state=3 rows written

  // THE RULING ON `session_id` REUSE, assigned to this ticket from ENC-1480.
  // Held as text so the tool prints it and the ticket comment and SPEC §5 copy
  // the same words. See `kSessionIdRuling`.
  std::string sessionIdRuling;
  std::string gapsNote;
};

// The two decided questions this ticket owed, in the words the SPEC gets.
extern const char* const kSessionIdRuling;
extern const char* const kGapsNote;

// ---------------------------------------------------------------------------
// Run it.
// ---------------------------------------------------------------------------
arrow::Result<Report> run(const Options& opts);

// ---------------------------------------------------------------------------
// The completeness read (D11), against a live server.
// ---------------------------------------------------------------------------
//
// Selects every `gma.ingest_windows` row for one `(symbol, kind)` — projection
// and a `WHERE` on the key prefix, no aggregate, no `FINAL`, no `argMax` — and
// hands them to `gma::data::isComplete`. The coverage test is in C++ because
// the DDL says so: it keeps this a plain selection on this side of the D2
// boundary.
arrow::Result<Completeness> queryCompleteness(const ch::Client& client,
                                              const std::string& symbol,
                                              const std::string& kind,
                                              std::int64_t reqFromNs,
                                              std::int64_t reqToNs);

// Row counts, for the report. NEVER used to decide completeness — a quiet
// window and a missing window have the same count (D11), which is why this
// returns alongside the verdict rather than instead of it.
arrow::Result<std::int64_t> rowCount(const ch::Client& client,
                                     const std::string& table,
                                     const std::string& symbol,
                                     std::int64_t fromNs, std::int64_t toNs);

// Formats a window as `[<iso>, <iso>)` for the report.
std::string formatWindow(std::int64_t fromNs, std::int64_t toNs);
std::string formatNs(std::int64_t ns);

}  // namespace gma::data::ingest
