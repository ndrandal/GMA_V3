#pragma once
//
// `gma.ingest_windows` — the read rule, as code (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D11;
//         docs/data-layer.md §8.8; and the ⚠ READ RULE block in
//         `data/schema/clickhouse.sql` above `CREATE TABLE gma.ingest_windows`,
//         which is the authority this file implements rather than reinvents.
//
// THE RULE, QUOTED FROM THE DDL:
//
//     isComplete(symbol, kind, [req_from, req_to)) :=
//         W := { [from_ts, to_ts) recorded for (symbol, kind)
//                whose NEWEST row by updated_at has state = 1 }
//         require  [req_from, req_to)  is covered by  union(W).
//         Any part of the request not covered -> NOT INGESTED -> FAIL (D11).
//
// Three things about it are load-bearing and each has a measured defect behind
// it (ENC-1480 measured all three against the running server):
//
//   1. RESOLVE FIRST, COVER SECOND. The newest row is resolved per RECORDED
//      WINDOW — `(symbol, kind, from_ts, to_ts)` — and only the survivors with
//      `state = 1` form W. A window re-opened by a re-ingest has to drop OUT
//      of W before coverage is tested. Filtering `WHERE state = 1` first
//      matches an older session's success row and reads a dropped partition
//      as complete.
//
//   2. COVERAGE, NOT A ROW. A recorded `[00:00, 12:00)` at state=1 says
//      nothing about `[12:00, 24:00)`. A rule that finds one state=1 row and
//      stops returns HALF A DAY AS IF WHOLE.
//
//   3. ROW COUNTS ARE NEVER CONSULTED. A quiet window and a missing window
//      have the same count, which is the entire reason the table exists.
//      `rows_inserted` is diagnostics (the DDL says so) and does not appear in
//      this file's logic.
//
// AND ONE THING THE DDL LEAVES TO THE READER, DECIDED HERE: WHAT A TIE IN
// `updated_at` MEANS. The column is `DateTime64(3)`, so two writes in the same
// millisecond tie, and the DDL only says a tie makes "the newest row"
// arbitrary. Arbitrary is not acceptable in the one table whose job is to not
// lie, so a tie resolves to the LOWEST state among the tied rows: a tie
// between `state=1` and `state=0` reads as in-flight, i.e. NOT complete.
// Pessimistic in the direction that fails the query rather than the direction
// that answers it over absent data. `TieResolvesToTheLowestState` is the test.
//
// THE COVERAGE TEST IS IN C++, NOT IN SQL, and that is the DDL's instruction
// rather than a convenience: it keeps the read a plain selection on this side
// of the D2 boundary, and an interval merge is easier to get right here. The
// SELECT that feeds this is projection + `WHERE` on the key prefix and nothing
// else.
//

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gma::data {

// One row of `gma.ingest_windows`, as selected. Only the five fields the rule
// reads are here; `session_id`, `rows_inserted`, `completed_at`, `source` and
// `source_ref` are provenance and diagnostics and are deliberately absent, so
// no future edit can quietly start consulting them.
struct IngestWindowRow {
  std::int64_t fromTsNs = 0;
  std::int64_t toTsNs   = 0;   // half-open [fromTsNs, toTsNs)
  std::uint8_t state    = 0;   // 0 in-flight, 1 complete, 2 failed, 3 invalidated
  std::int64_t updatedAtMs = 0;
};

struct Interval {
  std::int64_t fromNs = 0;
  std::int64_t toNs   = 0;  // half-open
  bool operator==(const Interval&) const noexcept = default;
};

// The verdict. `complete` is the answer; `gap` is the FIRST uncovered
// subinterval of the request, present exactly when `complete` is false, and it
// exists so the refusal can say what is missing rather than only that
// something is.
struct Completeness {
  bool complete = false;
  std::optional<Interval> gap;
  // The windows that survived step 1 — resolved newest-by-updated_at and
  // state=1 — merged. Reported so a refusal can print what IS held, which is
  // the difference between "we have nothing" and "we have 68 seconds of it".
  std::vector<Interval> covered;
  // How many recorded windows were resolved, and how many of those survived.
  // A request that fails with `resolved > 0 && survived == 0` is a re-ingest
  // in flight; one that fails with `resolved == 0` was never ingested. Same
  // refusal, different cause, and the operator needs to know which.
  std::size_t resolved = 0;
  std::size_t survived = 0;
};

// The rule. `rows` is every row selected for one `(symbol, kind)` — unordered;
// this does the grouping itself rather than trusting a SQL `ORDER BY`.
Completeness isComplete(const std::vector<IngestWindowRow>& rows,
                        std::int64_t reqFromNs, std::int64_t reqToNs);

// Merge half-open intervals, joining adjacent ones. Exposed because
// "`union(W)` so that a seven-day request over seven daily windows is
// complete" is a claim worth testing directly.
std::vector<Interval> mergeIntervals(std::vector<Interval> in);

}  // namespace gma::data
