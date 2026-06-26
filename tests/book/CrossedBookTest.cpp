// ENC-807 (L20): crossed-book coverage + a stable applyTrade consume
// characterization.
//
// The review flagged that addImpl never checks best-bid < best-ask and that
// checkInvariants() does not detect a crossed book, yet no test ever crossed
// one. These tests pin the ACTUAL current behavior (a crossed book is accepted
// and passes checkInvariants) and characterize that a single applyTrade()
// consumes its level exactly once — the property the C1 double-consume bug
// (ENC-785, fixed in the WsFeedClient routing layer) violates end-to-end.

#include "gma/book/OrderBook.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace gma;

namespace {
Order mkOrder(uint64_t id, Side side, int64_t ticks, uint64_t size) {
  Order o;
  o.id = id;
  o.side = side;
  o.price = Price{ticks};
  o.size = size;
  return o;
}
} // namespace

// ---------------------------------------------------------------------------
// A crossed book (best bid above best ask) is silently accepted; checkInvariants
// does NOT flag the crossing. Documents the L20 gap; a future crossed-book
// guard should flip the checkInvariants expectation.
// ---------------------------------------------------------------------------
TEST(CrossedBookTest, CrossedBookIsAcceptedAndNotFlagged) {
  OrderBook ob;
  EXPECT_TRUE(ob.applyAdd(mkOrder(1, Side::Bid, 105, 50)));
  EXPECT_TRUE(ob.applyAdd(mkOrder(2, Side::Ask, 100, 30)));

  auto bb = ob.bestBid();
  auto ba = ob.bestAsk();
  ASSERT_TRUE(bb.has_value());
  ASSERT_TRUE(ba.has_value());
  // Book is crossed: best bid (105) strictly above best ask (100).
  EXPECT_GT(bb->ticks, ba->ticks);

  // Current behavior: internal invariants pass despite the cross (no crossed-
  // book detection). Pinned so the gap can't silently change.
  std::string why;
  EXPECT_TRUE(ob.checkInvariants(&why))
      << "checkInvariants unexpectedly flagged the crossed book: " << why;
}

// ---------------------------------------------------------------------------
// One applyTrade() consumes its price level exactly once. Two resting bids at
// the same level (100 + 50 = 150); a 30-share sell-aggressor trade reduces the
// level to 120 — never 90 (which a double-consume would yield).
// ---------------------------------------------------------------------------
TEST(CrossedBookTest, ApplyTradeConsumesLevelExactlyOnce) {
  OrderBook ob;
  ASSERT_TRUE(ob.applyAdd(mkOrder(1, Side::Bid, 100, 100)));
  ASSERT_TRUE(ob.applyAdd(mkOrder(2, Side::Bid, 100, 50)));
  ASSERT_EQ(ob.levelSize(Side::Bid, Price{100}), 150u);

  // Sell aggressor hits the passive bid side.
  EXPECT_TRUE(ob.applyTrade(Price{100}, 30, Aggressor::Sell));

  EXPECT_EQ(ob.levelSize(Side::Bid, Price{100}), 120u)
      << "applyTrade must consume exactly once (150 - 30), not twice";
  EXPECT_EQ(ob.bestBidSize(), 120u);
}

// ---------------------------------------------------------------------------
// A zero-share trade is a safe no-op at the book layer (guards qty==0): no
// consume, returns false. This is the OrderBook-level counterpart to the ITCH
// zero-share phantom-event gap (ItchSemanticTest).
// ---------------------------------------------------------------------------
TEST(CrossedBookTest, ZeroShareTradeIsNoOp) {
  OrderBook ob;
  ASSERT_TRUE(ob.applyAdd(mkOrder(1, Side::Bid, 100, 75)));

  EXPECT_FALSE(ob.applyTrade(Price{100}, 0, Aggressor::Sell));
  EXPECT_EQ(ob.levelSize(Side::Bid, Price{100}), 75u);
}
