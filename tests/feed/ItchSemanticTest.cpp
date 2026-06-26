// ENC-807 (L20): ITCH semantic-malformation coverage the suite lacked.
//
// ItchAdapterTest covers broken JSON and the happy paths. This file pins the
// SEMANTIC malformations the review flagged as untested: untyped `price`
// silently coercing to 0.0, and zero-share execute/cancel emitting phantom
// events. Where Lane A (ENC-785 / ENC-796) is changing the production code, the
// tests assert higher-level invariants and any input that would currently
// abort() (the IsNumber()/GetUint64() bug, M2/ENC-796) is GUARDED behind a
// GTEST_SKIP with a TODO so it documents the gap without crashing the binary.
//
// TODO(ENC-785): the order_executed *double-consume* (C1) is a routing bug in
// WsFeedClient::dispatchEvent (it applies BOTH the explicit delete/update AND
// the trade-consume). It is not observable at the ItchAdapter.translate() layer
// exercised here — translate() merely emits the events. The end-to-end
// "execution consumes a level exactly once" regression test belongs with Lane
// A's dispatch fix; adding it here against unmerged routing would be a
// knowingly-failing test. See CrossedBookTest.cpp for the OrderBook-level
// consume characterization that IS stable across that fix.

#include "gma/feed/ItchAdapter.hpp"
#include "gma/feed/FeedEvent.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gma;
using namespace gma::feed;

namespace {

template <typename T>
size_t countEvents(const std::vector<FeedEvent>& events) {
  size_t n = 0;
  for (const auto& e : events)
    if (std::holds_alternative<T>(e)) ++n;
  return n;
}

template <typename T>
const T& firstEvent(const std::vector<FeedEvent>& events) {
  for (const auto& e : events)
    if (std::holds_alternative<T>(e)) return std::get<T>(e);
  throw std::runtime_error("event not found");
}

// Add a resting order via raw JSON and discard the resulting events.
void seedOrder(ItchAdapter& a, const char* stock, uint64_t ref,
               const char* side, uint64_t shares, double price) {
  std::string msg = std::string(R"({"type":"add_order","stock":")") + stock +
    R"(","orderRef":)" + std::to_string(ref) + R"(,"side":")" + side +
    R"(","shares":)" + std::to_string(shares) + R"(,"price":)" +
    std::to_string(price) + "}";
  a.translate(msg);
}

} // namespace

// ---------------------------------------------------------------------------
// Untyped price coerces to 0.0 (CURRENT behavior — documents the L20 gap).
// routeTrade only checks HasMember("price"), never its type, and parsePrice()
// returns 0.0 for a non-string/non-number value. A boolean price therefore
// produces a phantom trade print at price 0.0 rather than being rejected.
// ---------------------------------------------------------------------------
TEST(ItchSemanticTest, UntypedPriceOnTradeCoercesToZero) {
  ItchAdapter adapter;
  // price is a JSON bool -> neither string nor number -> parsePrice() == 0.0.
  auto events = adapter.translate(
      R"({"type":"trade","stock":"X","price":true,"shares":10})");

  ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);
  EXPECT_DOUBLE_EQ(firstEvent<ObTradeEvent>(events).price, 0.0)
      << "untyped price must currently coerce to 0.0 (documented coercion gap)";
}

TEST(ItchSemanticTest, UntypedPriceOnAddOrderCoercesToZero) {
  ItchAdapter adapter;
  auto events = adapter.translate(
      R"({"type":"add_order","stock":"X","orderRef":7,"side":"B","shares":5,"price":true})");

  ASSERT_EQ(countEvents<ObAddEvent>(events), 1u);
  EXPECT_DOUBLE_EQ(firstEvent<ObAddEvent>(events).price, 0.0);
}

// ---------------------------------------------------------------------------
// A string-typed numeric field that fails the IsNumber() guard is safely
// REJECTED (no event, no crash). This is the safe half of the M2 contract.
// ---------------------------------------------------------------------------
TEST(ItchSemanticTest, StringTypedSharesIsRejectedNotCrashing) {
  ItchAdapter adapter;
  // shares as a string fails !doc["shares"].IsNumber() -> early return.
  auto events = adapter.translate(
      R"({"type":"add_order","stock":"X","orderRef":8,"side":"B","shares":"100","price":10.0})");
  EXPECT_TRUE(events.empty());
}

// ---------------------------------------------------------------------------
// Zero-share execute emits PHANTOM events (CURRENT behavior — L20 gap).
// execShares=0 takes the partial-fill branch: remaining is unchanged yet an
// ObUpdateEvent fires, plus a zero-size ObTradeEvent and a zero-volume tick.
// Pinned so the phantom-event behavior can't change silently; a future fix to
// reject zero-share executes should update this test.
// ---------------------------------------------------------------------------
TEST(ItchSemanticTest, ZeroShareExecuteEmitsPhantomEvents) {
  ItchAdapter adapter;
  seedOrder(adapter, "PH", 1, "B", 100, 150.0);

  auto events = adapter.translate(
      R"({"type":"order_executed","orderRef":1,"shares":0})");

  // Phantom: an update with the SAME remaining size, a zero-size trade, a tick.
  ASSERT_EQ(countEvents<ObUpdateEvent>(events), 1u);
  const auto& upd = firstEvent<ObUpdateEvent>(events);
  ASSERT_TRUE(upd.newSize.has_value());
  EXPECT_EQ(*upd.newSize, 100u) << "remaining unchanged -> phantom update";

  ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);
  EXPECT_EQ(firstEvent<ObTradeEvent>(events).size, 0u) << "phantom zero-size trade";

  EXPECT_EQ(countEvents<TickEvent>(events), 1u);
}

// ---------------------------------------------------------------------------
// Zero-share cancel likewise emits a phantom no-op update (CURRENT behavior).
// ---------------------------------------------------------------------------
TEST(ItchSemanticTest, ZeroShareCancelEmitsPhantomUpdate) {
  ItchAdapter adapter;
  seedOrder(adapter, "PH2", 2, "S", 80, 200.0);

  auto events = adapter.translate(
      R"({"type":"order_cancel","orderRef":2,"shares":0})");

  ASSERT_EQ(countEvents<ObUpdateEvent>(events), 1u);
  EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);
  EXPECT_EQ(*firstEvent<ObUpdateEvent>(events).newSize, 80u);
}

// ---------------------------------------------------------------------------
// M2 / ENC-796: malformed-but-numeric fields (fractional orderRef, negative
// shares) used to pass IsNumber() and then hit GetUint64(), which asserts and
// abort()s in a debug/test build. The fix gates the typed getters behind
// IsUint64()/IsInt(), so such frames are rejected (no events) without crashing.
// ---------------------------------------------------------------------------
TEST(ItchSemanticTest, MalformedNumericFieldsRejectedNotAborted) {
  ItchAdapter adapter;

  // Fractional orderRef on an add must not produce a book mutation (and must
  // not abort the process).
  auto addEvents = adapter.translate(
      R"({"type":"add_order","stock":"X","orderRef":1.5,"side":"B","shares":5,"price":10})");
  EXPECT_EQ(countEvents<ObAddEvent>(addEvents), 0u);

  // Negative shares on an execute must not consume the book (and must not abort).
  seedOrder(adapter, "X", 1, "B", 5, 10.0);
  auto execEvents = adapter.translate(
      R"({"type":"order_executed","orderRef":1,"shares":-3})");
  EXPECT_EQ(countEvents<ObUpdateEvent>(execEvents), 0u);
  EXPECT_EQ(countEvents<ObDeleteEvent>(execEvents), 0u);
}
