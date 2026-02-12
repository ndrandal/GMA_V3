#include "gma/book/OrderBook.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/book/DepthTypes.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <atomic>

using namespace gma;

// ===================== OrderBook unit tests =====================

TEST(OrderBookTest, AddAndBestBidAsk) {
    OrderBook ob;
    Order bid; bid.id=1; bid.side=Side::Bid; bid.price=Price{100}; bid.size=50;
    Order ask; ask.id=2; ask.side=Side::Ask; ask.price=Price{101}; ask.size=30;

    EXPECT_TRUE(ob.applyAdd(bid));
    EXPECT_TRUE(ob.applyAdd(ask));

    auto bb = ob.bestBid();
    auto ba = ob.bestAsk();
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_EQ(bb->ticks, 100);
    EXPECT_EQ(ba->ticks, 101);
    EXPECT_EQ(ob.bestBidSize(), 50u);
    EXPECT_EQ(ob.bestAskSize(), 30u);
}

TEST(OrderBookTest, EmptyBookReturnsNullopt) {
    OrderBook ob;
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.bestAsk().has_value());
    EXPECT_EQ(ob.bestBidSize(), 0u);
    EXPECT_EQ(ob.bestAskSize(), 0u);
}

TEST(OrderBookTest, UpdateSize) {
    OrderBook ob;
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{100}; o.size=50;
    ob.applyAdd(o);

    EXPECT_TRUE(ob.applyUpdate(1, std::nullopt, std::optional<uint64_t>(75)));
    EXPECT_EQ(ob.bestBidSize(), 75u);
}

TEST(OrderBookTest, UpdatePrice) {
    OrderBook ob;
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{100}; o.size=50;
    ob.applyAdd(o);

    EXPECT_TRUE(ob.applyUpdate(1, std::optional<Price>(Price{105}), std::nullopt));
    EXPECT_EQ(ob.bestBid()->ticks, 105);
    EXPECT_EQ(ob.bestBidSize(), 50u);
    EXPECT_EQ(ob.levelSize(Side::Bid, Price{100}), 0u);
}

TEST(OrderBookTest, UpdateSizeToZeroRemovesOrder) {
    OrderBook ob;
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{100}; o.size=50;
    ob.applyAdd(o);

    EXPECT_TRUE(ob.applyUpdate(1, std::nullopt, std::optional<uint64_t>(0)));
    EXPECT_FALSE(ob.bestBid().has_value());
}

TEST(OrderBookTest, Delete) {
    OrderBook ob;
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{100}; o.size=50;
    ob.applyAdd(o);

    EXPECT_TRUE(ob.applyDelete(1));
    EXPECT_FALSE(ob.bestBid().has_value());
    EXPECT_FALSE(ob.applyDelete(1)); // double delete
}

TEST(OrderBookTest, MultipleLevels) {
    OrderBook ob;
    Order b1; b1.id=1; b1.side=Side::Bid; b1.price=Price{100}; b1.size=10;
    Order b2; b2.id=2; b2.side=Side::Bid; b2.price=Price{99};  b2.size=20;
    Order b3; b3.id=3; b3.side=Side::Bid; b3.price=Price{101}; b3.size=5;
    ob.applyAdd(b1);
    ob.applyAdd(b2);
    ob.applyAdd(b3);

    EXPECT_EQ(ob.bestBid()->ticks, 101);  // highest bid
    EXPECT_EQ(ob.bestBidSize(), 5u);
}

TEST(OrderBookTest, MultipleOrdersSameLevel) {
    OrderBook ob;
    Order o1; o1.id=1; o1.side=Side::Bid; o1.price=Price{100}; o1.size=10;
    Order o2; o2.id=2; o2.side=Side::Bid; o2.price=Price{100}; o2.size=20;
    ob.applyAdd(o1);
    ob.applyAdd(o2);

    EXPECT_EQ(ob.levelSize(Side::Bid, Price{100}), 30u);
    EXPECT_EQ(ob.bestBidSize(), 30u);
}

TEST(OrderBookTest, TradeConsumesFromPassiveSide) {
    OrderBook ob;
    Order ask; ask.id=1; ask.side=Side::Ask; ask.price=Price{100}; ask.size=50;
    ob.applyAdd(ask);

    EXPECT_TRUE(ob.applyTrade(Price{100}, 30, Aggressor::Buy));
    EXPECT_EQ(ob.bestAskSize(), 20u);

    EXPECT_TRUE(ob.applyTrade(Price{100}, 20, Aggressor::Buy));
    EXPECT_FALSE(ob.bestAsk().has_value()); // fully consumed
}

TEST(OrderBookTest, TradeZeroQtyReturnsFalse) {
    OrderBook ob;
    Order ask; ask.id=1; ask.side=Side::Ask; ask.price=Price{100}; ask.size=50;
    ob.applyAdd(ask);
    EXPECT_FALSE(ob.applyTrade(Price{100}, 0, Aggressor::Buy));
}

TEST(OrderBookTest, SnapshotPerOrder) {
    OrderBook ob;
    // Pre-populate
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{50}; o.size=10;
    ob.applyAdd(o);

    // Snapshot replaces everything
    std::vector<Order> snap;
    Order s1; s1.id=10; s1.side=Side::Bid; s1.price=Price{100}; s1.size=25;
    Order s2; s2.id=11; s2.side=Side::Ask; s2.price=Price{101}; s2.size=15;
    snap.push_back(s1);
    snap.push_back(s2);
    ob.applySnapshotPerOrder(snap);

    EXPECT_EQ(ob.bestBid()->ticks, 100);
    EXPECT_EQ(ob.bestAsk()->ticks, 101);
    EXPECT_EQ(ob.bestBidSize(), 25u);
    EXPECT_EQ(ob.bestAskSize(), 15u);
    // Old order gone
    EXPECT_EQ(ob.levelSize(Side::Bid, Price{50}), 0u);
}

TEST(OrderBookTest, SnapshotAggregated) {
    OrderBook ob;
    std::vector<LevelSnapshotEntry> levels;
    levels.push_back(LevelSnapshotEntry{Side::Bid, Price{100}, 50, std::nullopt});
    levels.push_back(LevelSnapshotEntry{Side::Ask, Price{101}, 30, 2});
    ob.applySnapshotAggregated(levels);

    EXPECT_EQ(ob.bestBidAggregated()->ticks, 100);
    EXPECT_EQ(ob.bestAskAggregated()->ticks, 101);
    EXPECT_EQ(ob.levelSizeAggregated(Side::Bid, Price{100}), 50u);
    EXPECT_EQ(ob.levelSizeAggregated(Side::Ask, Price{101}), 30u);
}

TEST(OrderBookTest, LevelSummary) {
    OrderBook ob;
    EXPECT_TRUE(ob.applyLevelSummary(Side::Bid, Price{100}, 50));
    EXPECT_EQ(ob.levelSizeAggregated(Side::Bid, Price{100}), 50u);

    // Update
    EXPECT_TRUE(ob.applyLevelSummary(Side::Bid, Price{100}, 75));
    EXPECT_EQ(ob.levelSizeAggregated(Side::Bid, Price{100}), 75u);

    // Zero size removes
    EXPECT_TRUE(ob.applyLevelSummary(Side::Bid, Price{100}, 0));
    EXPECT_EQ(ob.levelSizeAggregated(Side::Bid, Price{100}), 0u);
}

TEST(OrderBookTest, Priority) {
    OrderBook ob;
    Order o1; o1.id=1; o1.side=Side::Bid; o1.price=Price{100}; o1.size=10; o1.priority=1;
    Order o2; o2.id=2; o2.side=Side::Bid; o2.price=Price{100}; o2.size=20; o2.priority=2;
    ob.applyAdd(o1);
    ob.applyAdd(o2);

    // Changing priority should move order to back
    EXPECT_TRUE(ob.applyPriority(1, 5));
    EXPECT_FALSE(ob.applyPriority(999, 5)); // nonexistent
}

TEST(OrderBookTest, ForEachLevel) {
    OrderBook ob;
    Order b1; b1.id=1; b1.side=Side::Bid; b1.price=Price{100}; b1.size=10;
    Order b2; b2.id=2; b2.side=Side::Bid; b2.price=Price{99};  b2.size=20;
    Order b3; b3.id=3; b3.side=Side::Bid; b3.price=Price{98};  b3.size=30;
    ob.applyAdd(b1);
    ob.applyAdd(b2);
    ob.applyAdd(b3);

    std::vector<std::pair<int64_t, uint64_t>> out;
    ob.forEachLevel(Side::Bid, 2, [&](Price p, uint64_t sz) {
        out.emplace_back(p.ticks, sz);
    });
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].first, 100); // best bid first
    EXPECT_EQ(out[1].first, 99);
}

TEST(OrderBookTest, CheckInvariantsValid) {
    OrderBook ob;
    Order o; o.id=1; o.side=Side::Bid; o.price=Price{100}; o.size=50;
    ob.applyAdd(o);

    std::string why;
    EXPECT_TRUE(ob.checkInvariants(&why));
}

TEST(OrderBookTest, ScopedAddAndDelete) {
    OrderBook ob;
    FeedScope scope{1, 1};
    Order o; o.id=42; o.side=Side::Ask; o.price=Price{200}; o.size=100;

    EXPECT_TRUE(ob.applyAdd(o, scope, false));
    EXPECT_EQ(ob.bestAsk()->ticks, 200);

    OrderKey key{42, 1, 1, false};
    EXPECT_TRUE(ob.applyDelete(key));
    EXPECT_FALSE(ob.bestAsk().has_value());
}

TEST(OrderBookTest, ScopedAddWithMissingId) {
    OrderBook ob;
    FeedScope scope{1, 1};
    Order o; o.id=0; o.side=Side::Bid; o.price=Price{100}; o.size=10;

    EXPECT_TRUE(ob.applyAdd(o, scope, true));
    EXPECT_EQ(ob.bestBid()->ticks, 100);
}

TEST(OrderBookTest, ApplyAddGetKey) {
    OrderBook ob;
    FeedScope scope{2, 3};
    Order o; o.id=0; o.side=Side::Bid; o.price=Price{100}; o.size=25;

    OrderKey key = ob.applyAddGetKey(o, scope, true);
    EXPECT_EQ(key.feedId, 2u);
    EXPECT_EQ(key.epoch, 3u);
    EXPECT_TRUE(key.synthetic);

    Side s; Price p;
    EXPECT_TRUE(ob.locate(key, s, p));
    EXPECT_EQ(s, Side::Bid);
    EXPECT_EQ(p.ticks, 100);
}

TEST(OrderBookTest, LocateNonExistent) {
    OrderBook ob;
    Side s; Price p;
    OrderKey key{999, 0, 0, false};
    EXPECT_FALSE(ob.locate(key, s, p));
}

// ===================== OrderBookManager unit tests =====================

TEST(OrderBookManagerTest, TickSizeDefaultAndCustom) {
    OrderBookManager mgr;
    // Default tick
    EXPECT_NEAR(mgr.getTickSize("XYZ"), 1e-4, 1e-10);

    mgr.setTickSize("XYZ", 0.01);
    EXPECT_NEAR(mgr.getTickSize("XYZ"), 0.01, 1e-10);
}

TEST(OrderBookManagerTest, TickConversion) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);

    Price p = mgr.toTicks("S", 1.23);
    EXPECT_EQ(p.ticks, 123);

    double d = mgr.toDouble("S", p);
    EXPECT_NEAR(d, 1.23, 1e-10);
}

TEST(OrderBookManagerTest, FeedSequencing) {
    OrderBookManager mgr;
    EXPECT_TRUE(mgr.onSeq("S", 1));  // first seq, accepted
    EXPECT_TRUE(mgr.onSeq("S", 2));  // in order
    EXPECT_FALSE(mgr.onSeq("S", 5)); // gap → stale
    EXPECT_TRUE(mgr.isStale("S"));
    EXPECT_FALSE(mgr.onSeq("S", 6)); // still stale
}

TEST(OrderBookManagerTest, FeedReset) {
    OrderBookManager mgr;
    mgr.onSeq("S", 1);
    mgr.onReset("S", 2);
    EXPECT_TRUE(mgr.isStale("S"));
    auto fs = mgr.getFeedState("S");
    EXPECT_EQ(fs.epoch, 2u);
    EXPECT_EQ(fs.lastSeq, 0u);
}

TEST(OrderBookManagerTest, QueryOnNonExistentSymbol) {
    OrderBookManager mgr;
    EXPECT_FALSE(mgr.bestBid("NONE").has_value());
    EXPECT_FALSE(mgr.bestAsk("NONE").has_value());
    EXPECT_EQ(mgr.bestBidSize("NONE"), 0u);
    EXPECT_EQ(mgr.bestAskSize("NONE"), 0u);
}

TEST(OrderBookManagerTest, AddAndQueryBestBidAsk) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    // Don't go through sequencing — just add directly
    EXPECT_TRUE(mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0));
    EXPECT_TRUE(mgr.onAdd("S", 2, Side::Ask, 1.01, 30, 0));

    auto bb = mgr.bestBid("S");
    auto ba = mgr.bestAsk("S");
    ASSERT_TRUE(bb.has_value());
    ASSERT_TRUE(ba.has_value());
    EXPECT_NEAR(*bb, 1.00, 1e-10);
    EXPECT_NEAR(*ba, 1.01, 1e-10);
}

TEST(OrderBookManagerTest, DepthN) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);
    mgr.onAdd("S", 2, Side::Bid, 0.99, 30, 0);
    mgr.onAdd("S", 3, Side::Ask, 1.01, 10, 0);

    std::vector<std::pair<double,uint64_t>> bids, asks;
    mgr.depthN("S", 5, bids, asks);
    ASSERT_EQ(bids.size(), 2u);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_NEAR(bids[0].first, 1.00, 1e-10);
    EXPECT_NEAR(bids[1].first, 0.99, 1e-10);
}

TEST(OrderBookManagerTest, DeleteAndUpdate) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);

    EXPECT_TRUE(mgr.onUpdate("S", 1, FeedScope{}, std::nullopt, std::optional<uint64_t>(75)));
    EXPECT_EQ(mgr.bestBidSize("S"), 75u);

    EXPECT_TRUE(mgr.onDelete("S", 1, FeedScope{}));
    EXPECT_FALSE(mgr.bestBid("S").has_value());
}

TEST(OrderBookManagerTest, StaleGateBlocksMutations) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    // Force stale via seq gap
    mgr.onSeq("S", 1);
    mgr.onSeq("S", 5); // gap → stale

    EXPECT_FALSE(mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0));
    EXPECT_FALSE(mgr.onUpdate("S", 1, FeedScope{}, std::nullopt, std::optional<uint64_t>(75)));
    EXPECT_FALSE(mgr.onDelete("S", 1, FeedScope{}));
}

TEST(OrderBookManagerTest, EventBusSubscribeAndReceive) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);

    std::vector<BookDelta> received;
    auto subId = mgr.subscribeDeltas("S", [&](const BookDelta& d) {
        received.push_back(d);
    });

    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].symbol, "S");
    EXPECT_EQ(received[0].seq, 1u);

    mgr.unsubscribeDeltas("S", subId);
    mgr.onAdd("S", 2, Side::Ask, 1.01, 30, 0);
    EXPECT_EQ(received.size(), 1u); // no more
}

TEST(OrderBookManagerTest, BuildSnapshot) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);
    mgr.onAdd("S", 2, Side::Ask, 1.01, 30, 0);

    auto snap = mgr.buildSnapshot("S", 5);
    EXPECT_EQ(snap.symbol, "S");
    ASSERT_EQ(snap.bids.size(), 1u);
    ASSERT_EQ(snap.asks.size(), 1u);
    EXPECT_EQ(snap.bids[0].first.ticks, 100);
    EXPECT_EQ(snap.asks[0].first.ticks, 101);
}

TEST(OrderBookManagerTest, ResolverPutAndGet) {
    OrderBookManager mgr;
    OrderKey key{42, 1, 1, false};
    mgr.resolverPut("S", "venue-key-1", key);

    auto got = mgr.resolverGet("S", "venue-key-1");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id, 42u);

    EXPECT_FALSE(mgr.resolverGet("S", "nonexistent").has_value());
    EXPECT_FALSE(mgr.resolverGet("OTHER", "venue-key-1").has_value());
}

TEST(OrderBookManagerTest, AssertInvariantsOnEmptySymbol) {
    OrderBookManager mgr;
    std::string why;
    // Book not found → false
    EXPECT_FALSE(mgr.assertInvariants("NONE", &why));
    EXPECT_EQ(why, "book not found");
}

TEST(OrderBookManagerTest, AssertInvariantsValid) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);

    std::string why;
    EXPECT_TRUE(mgr.assertInvariants("S", &why));
}

TEST(OrderBookManagerTest, DumpLadder) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);
    mgr.onAdd("S", 1, Side::Bid, 1.00, 50, 0);

    std::string dump = mgr.dumpLadder("S");
    EXPECT_NE(dump.find("DUMP S"), std::string::npos);
    EXPECT_NE(dump.find("BIDS"), std::string::npos);
    EXPECT_NE(dump.find("ASKS"), std::string::npos);
}

TEST(OrderBookManagerTest, ConcurrentAccess) {
    OrderBookManager mgr;
    mgr.setTickSize("S", 0.01);

    std::atomic<bool> go{false};
    auto writer = [&](int base) {
        while (!go.load()) {}
        for (int i = 0; i < 100; ++i) {
            uint64_t id = static_cast<uint64_t>(base * 1000 + i);
            mgr.onAdd("S", id, Side::Bid, 1.00, 10, 0);
        }
    };

    auto reader = [&]() {
        while (!go.load()) {}
        for (int i = 0; i < 100; ++i) {
            mgr.bestBid("S");
            mgr.bestAsk("S");
            mgr.bestBidSize("S");
        }
    };

    std::thread t1(writer, 1);
    std::thread t2(writer, 2);
    std::thread t3(reader);
    std::thread t4(reader);

    go.store(true);
    t1.join(); t2.join(); t3.join(); t4.join();

    // Just verifying no crashes/races
    EXPECT_TRUE(mgr.assertInvariants("S"));
}
