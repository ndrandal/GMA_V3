//
// The `gma.ingest_windows` read rule (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D11; docs/data-layer.md §8.8;
//         the ⚠ READ RULE block in data/schema/clickhouse.sql.
//
// WHY THESE TESTS ARE PURE AND NOT AGAINST THE SERVER. The rule is the thing
// that can be wrong, and it is a function of rows. Driving it through
// ClickHouse would make the suite SKIP when no server is up — and a skip looks
// exactly like a pass (ENC-1095's three sessions reading a green 188/188 as
// renderer coverage). The live exercise is `gma_ingest verify`, whose exit code
// is the verdict and which cannot be mistaken for a passing test when the
// server is absent: it exits 2.
//
// EACH CASE BELOW IS A FAILURE SOMEONE MEASURED, not a shape someone imagined.
// `TheNewestRowWinsPerRecordedWindow` and
// `ResolvingPerFromTsAloneAnswersHalfADayAsWhole` are the two defects ENC-1480
// measured against the running server; `TieResolvesToTheLowestState` is the
// one the DDL named and left to the reader.
//

#include "gma/data/IngestWindows.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace gma::data {
namespace {

constexpr std::int64_t kH = 3600000000000LL;  // one hour in ns
constexpr std::int64_t kD = 24 * kH;

IngestWindowRow row(std::int64_t from, std::int64_t to, std::uint8_t state,
                    std::int64_t updatedAtMs) {
  IngestWindowRow r;
  r.fromTsNs    = from;
  r.toTsNs      = to;
  r.state       = state;
  r.updatedAtMs = updatedAtMs;
  return r;
}

// ---------------------------------------------------------------------------
// The happy shape, and the boundaries of it.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, ARecordedWindowCoversItselfExactly) {
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 100)};
  const auto v = isComplete(rows, 0, kD);
  EXPECT_TRUE(v.complete);
  EXPECT_FALSE(v.gap.has_value());
  EXPECT_EQ(v.resolved, 1u);
  EXPECT_EQ(v.survived, 1u);
}

TEST(IngestWindowsReadRule, OneNanosecondPastTheRecordedEndIsRefused) {
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 100)};
  const auto v = isComplete(rows, 0, kD + 1);
  EXPECT_FALSE(v.complete);
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, kD);
  EXPECT_EQ(v.gap->toNs, kD + 1);
}

TEST(IngestWindowsReadRule, OneNanosecondBeforeTheRecordedStartIsRefused) {
  const std::vector<IngestWindowRow> rows{row(kH, kD, 1, 100)};
  const auto v = isComplete(rows, kH - 1, kD);
  EXPECT_FALSE(v.complete);
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, kH - 1);
  EXPECT_EQ(v.gap->toNs, kH);
}

TEST(IngestWindowsReadRule, StrictlyInsideSucceeds) {
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 100)};
  EXPECT_TRUE(isComplete(rows, kH, 2 * kH).complete);
}

// ---------------------------------------------------------------------------
// THE CASE THIS TICKET EXISTS TO DEMONSTRATE, in miniature: the honest answer
// to "do you have 2022-06-10?" when 68 seconds of it were delivered.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, AFullDayRequestOverASixtyEightSecondWindowIsRefused) {
  // [12:30:01.1, 12:31:09.37) — the real shape of this sample's mbo pull.
  const std::int64_t from = 45001100846675LL;
  const std::int64_t to   = 45069372036574LL;
  const std::vector<IngestWindowRow> rows{row(from, to, 1, 100)};

  const auto day = isComplete(rows, 0, kD);
  EXPECT_FALSE(day.complete) << "a 68-second window must not cover a day";
  ASSERT_TRUE(day.gap.has_value());
  EXPECT_EQ(day.gap->fromNs, 0) << "the first hole starts at the day's start";
  EXPECT_EQ(day.gap->toNs, from);

  // And the window the pull itself REQUESTED, [12:30, 14:00), is not covered
  // either — which is the sharper statement, because `limit: 1000` truncated
  // the delivery.
  const auto requested = isComplete(rows, 45000000000000LL, 50400000000000LL);
  EXPECT_FALSE(requested.complete);

  // The in-window query succeeds, so the refusal above is about coverage and
  // not about the rule being broken.
  EXPECT_TRUE(isComplete(rows, from, to).complete);
}

// ---------------------------------------------------------------------------
// union(W): adjacent half-open windows join; a hole anywhere fails.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, SevenAdjacentDailyWindowsCoverASevenDayRequest) {
  std::vector<IngestWindowRow> rows;
  for (int i = 0; i < 7; ++i) {
    rows.push_back(row(i * kD, (i + 1) * kD, 1, 100 + i));
  }
  EXPECT_TRUE(isComplete(rows, 0, 7 * kD).complete);
  EXPECT_EQ(isComplete(rows, 0, 7 * kD).survived, 7u);
}

TEST(IngestWindowsReadRule, AHoleInTheMiddleFailsAndNamesIt) {
  std::vector<IngestWindowRow> rows;
  for (int i = 0; i < 7; ++i) {
    if (i == 3) continue;  // the hole
    rows.push_back(row(i * kD, (i + 1) * kD, 1, 100 + i));
  }
  const auto v = isComplete(rows, 0, 7 * kD);
  EXPECT_FALSE(v.complete);
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, 3 * kD);
  EXPECT_EQ(v.gap->toNs, 4 * kD) << "the gap ends where the next held window starts";
}

TEST(IngestWindowsReadRule, OverlappingWindowsMergeRatherThanDoubleCount) {
  const std::vector<IngestWindowRow> rows{row(0, 2 * kD, 1, 100),
                                          row(kD, 3 * kD, 1, 100)};
  EXPECT_TRUE(isComplete(rows, 0, 3 * kD).complete);
  EXPECT_EQ(isComplete(rows, 0, 3 * kD).covered.size(), 1u);
}

// ---------------------------------------------------------------------------
// ENC-1480's TWO MEASURED DEFECTS.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, TheNewestRowWinsPerRecordedWindow) {
  // Measured on the live server during ENC-1480: (session 1, state=1) followed
  // by (session 7, state=0) — a re-ingest in flight over a DROPPED partition.
  // `WHERE state = 1` matches one row and the window reads complete while its
  // data is gone.
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 1000), row(0, kD, 0, 2000)};
  const auto v = isComplete(rows, 0, kD);
  EXPECT_FALSE(v.complete) << "a re-ingest in flight must drop OUT of W";
  EXPECT_EQ(v.resolved, 1u) << "one RECORDED window, two rows";
  EXPECT_EQ(v.survived, 0u);

  // The naive rule, shown failing on the same input, so this test cannot pass
  // for the wrong reason: at least one row DOES have state = 1.
  bool anyStateOne = false;
  for (const auto& r : rows) anyStateOne = anyStateOne || r.state == 1;
  EXPECT_TRUE(anyStateOne)
      << "if no row had state=1 this case would prove nothing about resolution";
}

TEST(IngestWindowsReadRule, ResolvingPerFromTsAloneAnswersHalfADayAsWhole) {
  // The DDL's defect 2: a recorded [00:00, 12:00) at state=1 says nothing about
  // [12:00, 24:00). Both rows share from_ts, so a rule keyed on (symbol, kind,
  // from_ts) alone would resolve them against each other and the NEWER would
  // decide a range the older never claimed.
  const std::vector<IngestWindowRow> rows{
      row(0, 12 * kH, 1, 2000),  // half a day, loaded, and NEWER
      row(0, kD, 0, 1000),       // the whole day, never finished
  };
  const auto v = isComplete(rows, 0, kD);
  EXPECT_FALSE(v.complete) << "half a day must not answer for a whole one";
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, 12 * kH);
  EXPECT_EQ(v.gap->toNs, kD);
  EXPECT_EQ(v.resolved, 2u) << "to_ts is in the resolution key, so these are "
                               "TWO recorded windows, not two rows of one";

  // And the half-day request it genuinely does cover still succeeds.
  EXPECT_TRUE(isComplete(rows, 0, 12 * kH).complete);
}

// ---------------------------------------------------------------------------
// The DDL's defect 3, which it named and left to the reader.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, TieResolvesToTheLowestState) {
  // updated_at is DateTime64(3), so two writes in one millisecond tie. The DDL
  // says only that a tie makes "the newest row" arbitrary. Arbitrary is not
  // acceptable here, so the lowest state wins: a tie between complete and
  // in-flight reads as NOT complete.
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 5000), row(0, kD, 0, 5000)};
  EXPECT_FALSE(isComplete(rows, 0, kD).complete);

  // Order-independence: the same two rows the other way round must give the
  // same answer, which is what "not arbitrary" means.
  const std::vector<IngestWindowRow> flipped{row(0, kD, 0, 5000), row(0, kD, 1, 5000)};
  EXPECT_FALSE(isComplete(flipped, 0, kD).complete);
}

TEST(IngestWindowsReadRule, StateTwoAndThreeContributeNothing) {
  for (const std::uint8_t state : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{3}}) {
    const std::vector<IngestWindowRow> rows{row(0, kD, state, 100)};
    const auto v = isComplete(rows, 0, kD);
    EXPECT_FALSE(v.complete) << "state " << static_cast<int>(state);
    EXPECT_EQ(v.survived, 0u) << "state " << static_cast<int>(state);
  }
  // state=1 on the same window IS complete, so the loop above is not vacuous.
  EXPECT_TRUE(isComplete({row(0, kD, 1, 100)}, 0, kD).complete);
}

TEST(IngestWindowsReadRule, AStateThreeInvalidationVoidsAnEarlierSuccess) {
  // The collateral-damage case: symbol B's window was complete, then symbol A's
  // re-ingest dropped the shared day partition and the job wrote B a state=3.
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 1000), row(0, kD, 3, 2000)};
  EXPECT_FALSE(isComplete(rows, 0, kD).complete);
}

// ---------------------------------------------------------------------------
// Absence, emptiness and malformed requests.
// ---------------------------------------------------------------------------

TEST(IngestWindowsReadRule, NoRowsIsRefusedAndSaysSoDistinctly) {
  const auto v = isComplete({}, 0, kD);
  EXPECT_FALSE(v.complete);
  EXPECT_EQ(v.resolved, 0u)
      << "resolved == 0 means NEVER INGESTED; resolved > 0 with survived == 0 "
         "means a re-ingest is in flight. Same refusal, different cause.";
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, 0);
  EXPECT_EQ(v.gap->toNs, kD);
}

TEST(IngestWindowsReadRule, AnInvertedOrEmptyRequestIsNotTriviallyComplete) {
  const std::vector<IngestWindowRow> rows{row(0, kD, 1, 100)};
  EXPECT_FALSE(isComplete(rows, kH, kH).complete) << "an empty request";
  EXPECT_FALSE(isComplete(rows, 2 * kH, kH).complete) << "an inverted request";
}

TEST(IngestWindowsReadRule, AnEmptyRecordedWindowCannotBridgeAHole) {
  // A recorded [t, t) covers nothing. If `mergeIntervals` kept it, it would sit
  // between two real windows and join them, manufacturing coverage of the hole
  // it sits in.
  const std::vector<IngestWindowRow> rows{
      row(0, kD, 1, 100),
      row(kD, kD, 1, 100),  // empty, exactly at the hole's start
      row(2 * kD, 3 * kD, 1, 100),
  };
  const auto v = isComplete(rows, 0, 3 * kD);
  EXPECT_FALSE(v.complete);
  ASSERT_TRUE(v.gap.has_value());
  EXPECT_EQ(v.gap->fromNs, kD);
  EXPECT_EQ(v.gap->toNs, 2 * kD);
}

TEST(IngestWindowsMerge, AdjacentHalfOpenIntervalsJoinAndDisjointOnesDoNot) {
  const auto joined = mergeIntervals({Interval{0, kD}, Interval{kD, 2 * kD}});
  ASSERT_EQ(joined.size(), 1u);
  EXPECT_EQ(joined[0], (Interval{0, 2 * kD}));

  const auto apart = mergeIntervals({Interval{0, kD}, Interval{kD + 1, 2 * kD}});
  EXPECT_EQ(apart.size(), 2u) << "one nanosecond of hole is a hole";

  const auto dropped = mergeIntervals({Interval{kD, kD}, Interval{2 * kD, kD}});
  EXPECT_TRUE(dropped.empty()) << "empty and inverted intervals are dropped";
}

// ---------------------------------------------------------------------------
// ANTI-VACUITY. `data/CMakeLists.txt` globs `tests/*.cpp` WITHOUT
// CONFIGURE_DEPENDS (ENC-1495), so a new test file that was never configured in
// is not compiled, and the binary then reports "0 tests from 0 test suites" at
// EXIT 0 — a pass that ran nothing. A test inside the file cannot detect the
// file's own absence, but it CAN detect the other half: cases silently removed
// from a file that IS compiled. The count is asserted here and the total is
// checked by hand against the baseline on every gate run.
// ---------------------------------------------------------------------------
TEST(IngestWindowsAntiVacuity, EveryCaseInThisFileIsRegistered) {
  if (std::string(GTEST_FLAG_GET(filter)) != "*") {
    GTEST_SKIP() << "a filter is active; the count would be about the filter";
  }
  const auto* unit = ::testing::UnitTest::GetInstance();
  int found = 0;
  for (int i = 0; i < unit->total_test_suite_count(); ++i) {
    const auto* suite = unit->GetTestSuite(i);
    const std::string name = suite->name();
    if (name.rfind("IngestWindows", 0) == 0) found += suite->total_test_count();
  }
  EXPECT_EQ(found, 18) << "the IngestWindows* suites in this file lost or gained "
                          "a case; update the number deliberately rather than "
                          "letting the count drift";
}

}  // namespace
}  // namespace gma::data
