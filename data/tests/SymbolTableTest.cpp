// SymbolTable — interning, id stability, and the reserved zero (ENC-1481).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D3.
//
// ENC-1102 note: SymbolTable is an ORDINARY OBJECT, not one of the repo's
// process-global registries (FunctionMap, NodeTypeRegistry, …), so these tests
// need no per-invocation key derivation — each one constructs its own table.
// The one process-global thing this binary touches is the function-local
// `static` inside each schema accessor, and it is immutable; `SchemaIsStable`
// in SchemaRoundTripTest.cpp pins that, and the suite is run under
// --gtest_repeat to prove it.

#include "gma/data/Record.hpp"
#include "gma/data/SymbolTable.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using gma::data::kInvalidSymbolId;
using gma::data::SymbolId;
using gma::data::SymbolTable;

TEST(SymbolTableTest, EmptyTableKnowsNothing) {
  SymbolTable t;
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.lookup("AAPL"), kInvalidSymbolId);
  EXPECT_FALSE(t.contains(kInvalidSymbolId));
  EXPECT_FALSE(t.contains(1));
  EXPECT_EQ(t.name(kInvalidSymbolId), "");
}

// The reason ids start at 1: RecordHeader zero-initializes symbolId/venueId,
// so a zero-based id space would make a default-constructed header silently
// name whichever symbol happened to be interned first.
TEST(SymbolTableTest, ZeroIsReservedAndNeverIssued) {
  SymbolTable t;
  const auto first = t.intern("AAPL");
  EXPECT_NE(first, kInvalidSymbolId);
  EXPECT_EQ(first, 1u);

  gma::data::RecordHeader fresh;
  EXPECT_EQ(fresh.symbolId, kInvalidSymbolId);
  EXPECT_EQ(fresh.venueId, kInvalidSymbolId);
  EXPECT_FALSE(t.contains(fresh.symbolId));
}

TEST(SymbolTableTest, InternIsIdempotentAndIdsAreStable) {
  SymbolTable t;
  const auto a1 = t.intern("AAPL");
  const auto m1 = t.intern("MSFT");
  const auto a2 = t.intern("AAPL");
  EXPECT_EQ(a1, a2);
  EXPECT_NE(a1, m1);
  EXPECT_EQ(t.size(), 2u);

  // Interning more must not move an existing id.
  for (int i = 0; i < 100; ++i) t.intern("FILLER" + std::to_string(i));
  EXPECT_EQ(t.intern("AAPL"), a1);
  EXPECT_EQ(t.name(a1), "AAPL");
  EXPECT_EQ(t.name(m1), "MSFT");
}

TEST(SymbolTableTest, LookupDoesNotIntern) {
  SymbolTable t;
  EXPECT_EQ(t.lookup("AAPL"), kInvalidSymbolId);
  EXPECT_EQ(t.size(), 0u);
  t.intern("AAPL");
  EXPECT_EQ(t.lookup("AAPL"), 1u);
  EXPECT_EQ(t.size(), 1u);
}

TEST(SymbolTableTest, UnknownIdResolvesToEmptyButContainsSaysWhy) {
  SymbolTable t;
  t.intern("AAPL");
  EXPECT_EQ(t.name(99), "");
  EXPECT_FALSE(t.contains(99));
  // name() flattens three different situations to ""; contains() is how a
  // caller tells "unknown id" from "a symbol whose text is empty".
  const auto blank = t.intern("");
  EXPECT_EQ(t.name(blank), "");
  EXPECT_TRUE(t.contains(blank));
}

// An empty ticker is a provider bug. Folding it onto kInvalidSymbolId would
// hide it; it gets an ordinary id so the caller can reject it loudly.
TEST(SymbolTableTest, EmptyStringGetsAnOrdinaryId) {
  SymbolTable t;
  const auto id = t.intern("");
  EXPECT_NE(id, kInvalidSymbolId);
  EXPECT_EQ(t.intern(""), id);
  EXPECT_EQ(t.size(), 1u);
}

TEST(SymbolTableTest, ClearResetsTheIdSpace) {
  SymbolTable t;
  t.intern("AAPL");
  t.intern("MSFT");
  t.clear();
  EXPECT_EQ(t.size(), 0u);
  EXPECT_EQ(t.lookup("AAPL"), kInvalidSymbolId);
  EXPECT_EQ(t.intern("MSFT"), 1u);  // ids restart
}

// Concurrent interning: the decode path may run on several threads, and the
// double-checked lock in intern() is the only thing keeping two threads from
// issuing two ids for one string. Asserts a unique id per distinct string and
// one id per string across threads — not merely "does not crash".
TEST(SymbolTableTest, ConcurrentInternIssuesOneIdPerString) {
  SymbolTable t;
  constexpr int kThreads = 8;
  constexpr int kSymbols = 200;

  std::vector<std::vector<SymbolId>> seen(kThreads);
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int th = 0; th < kThreads; ++th) {
    threads.emplace_back([&, th] {
      seen[th].reserve(kSymbols);
      for (int i = 0; i < kSymbols; ++i) {
        seen[th].push_back(t.intern("SYM" + std::to_string(i)));
      }
    });
  }
  for (auto& th : threads) th.join();

  ASSERT_EQ(t.size(), static_cast<std::size_t>(kSymbols));
  // Every thread must have observed the SAME id for a given string.
  for (int th = 1; th < kThreads; ++th) {
    EXPECT_EQ(seen[th], seen[0]) << "thread " << th << " disagrees on ids";
  }
  // And the ids are a permutation of 1..kSymbols with no duplicates.
  std::unordered_set<SymbolId> unique(seen[0].begin(), seen[0].end());
  EXPECT_EQ(unique.size(), static_cast<std::size_t>(kSymbols));
  for (auto id : unique) {
    EXPECT_GE(id, 1u);
    EXPECT_LE(id, static_cast<SymbolId>(kSymbols));
    EXPECT_TRUE(t.contains(id));
  }
}

// SPEC D8: the fixed global 1e-9 scale.
TEST(FixedPointTest, ScaleIsGlobalAndExact) {
  EXPECT_EQ(gma::data::kPriceScale, 1'000'000'000LL);
  EXPECT_EQ(gma::data::kNoPrice, INT64_MIN);
  EXPECT_EQ(gma::data::toFixed(1.0), 1'000'000'000LL);
  EXPECT_EQ(gma::data::toFixed(-1.5), -1'500'000'000LL);
  EXPECT_EQ(gma::data::toFixed(0.0), 0LL);
  EXPECT_DOUBLE_EQ(gma::data::toDouble(1'234'567'890LL), 1.23456789);
}

// WHY SPEC D8 insists on "an exact integer path from DBN with no double round
// trip". A 1e-9-scaled price past 2^53 cannot survive a double, so a
// normalizer that routed DBN's own fixed point through toDouble/toFixed would
// corrupt large prices silently. The second half of this test is the reason it
// would not show up in a smoke test: an ordinary equity price round-trips
// perfectly, so the bug is invisible until it is not.
TEST(FixedPointTest, DoubleRoundTripIsLossyPastTwoToThe53) {
  const std::int64_t beyondDouble = 9'007'199'254'740'993LL;  // 2^53 + 1
  EXPECT_NE(gma::data::toFixed(gma::data::toDouble(beyondDouble)), beyondDouble);

  const std::int64_t aapl = 190'125'000'000LL;  // $190.125 at 1e-9
  EXPECT_EQ(gma::data::toFixed(gma::data::toDouble(aapl)), aapl);
}
