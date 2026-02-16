#include "gma/ob/ObMaterializer.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace gma::ob;

// Build a known test snapshot
static Snapshot makeTestSnap() {
    Snapshot s;
    // Bids: 100@10, 99@20, 98@30 (best→worse)
    s.bids.levels = {
        {100.0, 10.0, 3, 1000.0},
        { 99.0, 20.0, 5, 1980.0},
        { 98.0, 30.0, 7, 2940.0},
    };
    // Asks: 101@15, 102@25, 103@35
    s.asks.levels = {
        {101.0, 15.0, 4, 1515.0},
        {102.0, 25.0, 6, 2550.0},
        {103.0, 35.0, 8, 3605.0},
    };
    s.meta.seq = 42;
    s.meta.epoch = 7;
    s.meta.stale = false;
    s.meta.bidLevels = 3;
    s.meta.askLevels = 3;
    s.meta.lastChangeMs = 1700000000000LL;
    return s;
}

static Snapshot makeEmptySnap() {
    return Snapshot{};
}

// ---------------------------------------------------------------
// bestPrice / bestSize
// ---------------------------------------------------------------

TEST(ObMaterializerTest, BestPriceBid) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(bestPrice(s, Side::Bid), 100.0);
}

TEST(ObMaterializerTest, BestPriceAsk) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(bestPrice(s, Side::Ask), 101.0);
}

TEST(ObMaterializerTest, BestSizeBid) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(bestSize(s, Side::Bid), 10.0);
}

TEST(ObMaterializerTest, BestPriceEmptyReturnsNaN) {
    auto s = makeEmptySnap();
    EXPECT_TRUE(std::isnan(bestPrice(s, Side::Bid)));
}

TEST(ObMaterializerTest, BestSizeEmptyReturnsZero) {
    auto s = makeEmptySnap();
    EXPECT_DOUBLE_EQ(bestSize(s, Side::Bid), 0.0);
}

// ---------------------------------------------------------------
// spread / mid
// ---------------------------------------------------------------

TEST(ObMaterializerTest, Spread) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(spread(s), 1.0); // 101 - 100
}

TEST(ObMaterializerTest, Mid) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(mid(s), 100.5); // (100 + 101) / 2
}

TEST(ObMaterializerTest, SpreadEmptyReturnsNaN) {
    auto s = makeEmptySnap();
    EXPECT_TRUE(std::isnan(spread(s)));
}

TEST(ObMaterializerTest, MidEmptyReturnsNaN) {
    auto s = makeEmptySnap();
    EXPECT_TRUE(std::isnan(mid(s)));
}

// ---------------------------------------------------------------
// levelIdx
// ---------------------------------------------------------------

TEST(ObMaterializerTest, LevelIdxValidIndex) {
    auto s = makeTestSnap();
    LevelIdx spec{Side::Bid, 2, Target::Price};
    EXPECT_DOUBLE_EQ(levelIdx(s, spec), 99.0);
}

TEST(ObMaterializerTest, LevelIdxOutOfRange) {
    auto s = makeTestSnap();
    LevelIdx spec{Side::Bid, 10, Target::Price};
    EXPECT_TRUE(std::isnan(levelIdx(s, spec)));
}

// ---------------------------------------------------------------
// cumLevels
// ---------------------------------------------------------------

TEST(ObMaterializerTest, CumLevelsSizeOverN) {
    auto s = makeTestSnap();
    // Sum sizes of top 2 bids: 10 + 20 = 30
    EXPECT_DOUBLE_EQ(cumLevels(s, Side::Bid, 2, Target::Size), 30.0);
}

TEST(ObMaterializerTest, CumLevelsNotionalOverAll) {
    auto s = makeTestSnap();
    // Sum notional of all 3 bids: 1000 + 1980 + 2940 = 5920
    EXPECT_DOUBLE_EQ(cumLevels(s, Side::Bid, 3, Target::Notional), 5920.0);
}

TEST(ObMaterializerTest, CumLevelsNClamped) {
    auto s = makeTestSnap();
    // N > levels.size() → clamp to actual size
    double all3 = cumLevels(s, Side::Ask, 3, Target::Size);
    double over = cumLevels(s, Side::Ask, 100, Target::Size);
    EXPECT_DOUBLE_EQ(all3, over);
}

TEST(ObMaterializerTest, CumLevelsEmptyReturnsZero) {
    auto s = makeEmptySnap();
    EXPECT_DOUBLE_EQ(cumLevels(s, Side::Bid, 5, Target::Size), 0.0);
}

// ---------------------------------------------------------------
// vwapLevels
// ---------------------------------------------------------------

TEST(ObMaterializerTest, VwapLevelsKnown) {
    auto s = makeTestSnap();
    // VWAP over bid levels 1-2: (100*10 + 99*20) / (10+20) = 2980/30
    double expected = (100.0*10.0 + 99.0*20.0) / 30.0;
    EXPECT_NEAR(vwapLevels(s, Side::Bid, {1, 2}), expected, 1e-10);
}

TEST(ObMaterializerTest, VwapLevelsEmptyReturnsNaN) {
    auto s = makeEmptySnap();
    EXPECT_TRUE(std::isnan(vwapLevels(s, Side::Bid, {1, 5})));
}

// ---------------------------------------------------------------
// vwapPriceBand
// ---------------------------------------------------------------

TEST(ObMaterializerTest, VwapPriceBandFilter) {
    auto s = makeTestSnap();
    // Ask levels at 101, 102, 103. Filter 101-102 only:
    // VWAP = (101*15 + 102*25) / (15+25) = (1515+2550)/40 = 4065/40
    double expected = (101.0*15.0 + 102.0*25.0) / 40.0;
    EXPECT_NEAR(vwapPriceBand(s, Side::Ask, 101.0, 102.0, 0.0), expected, 1e-10);
}

TEST(ObMaterializerTest, VwapPriceBandNoMatchReturnsNaN) {
    auto s = makeTestSnap();
    EXPECT_TRUE(std::isnan(vwapPriceBand(s, Side::Bid, 200.0, 300.0, 0.0)));
}

// ---------------------------------------------------------------
// rangeIdxReduce
// ---------------------------------------------------------------

TEST(ObMaterializerTest, RangeIdxReduceSum) {
    auto s = makeTestSnap();
    RangeIdxSpec spec{Side::Bid, {1, 3}, Reduce::Sum, Target::Size};
    EXPECT_DOUBLE_EQ(rangeIdxReduce(s, spec), 60.0); // 10+20+30
}

TEST(ObMaterializerTest, RangeIdxReduceAvg) {
    auto s = makeTestSnap();
    RangeIdxSpec spec{Side::Bid, {1, 3}, Reduce::Avg, Target::Size};
    EXPECT_DOUBLE_EQ(rangeIdxReduce(s, spec), 20.0); // 60/3
}

// ---------------------------------------------------------------
// rangePxReduce
// ---------------------------------------------------------------

TEST(ObMaterializerTest, RangePxReduceFilterByPrice) {
    auto s = makeTestSnap();
    // Ask levels: 101@15, 102@25, 103@35. Price band 101-102:
    RangePxSpec spec{Side::Ask, 101.0, 102.0, Reduce::Sum, Target::Size};
    EXPECT_DOUBLE_EQ(rangePxReduce(s, spec, 0.0), 40.0); // 15+25
}

// ---------------------------------------------------------------
// imbalanceLevels / imbalanceBand
// ---------------------------------------------------------------

TEST(ObMaterializerTest, ImbalanceLevels) {
    auto s = makeTestSnap();
    // Levels 1-2: bid=10+20=30, ask=15+25=40, imb=(30-40)/(30+40) = -10/70
    double expected = (30.0 - 40.0) / 70.0;
    EXPECT_NEAR(imbalanceLevels(s, {1, 2}), expected, 1e-10);
}

TEST(ObMaterializerTest, ImbalanceBand) {
    auto s = makeTestSnap();
    // Price band 99-101: bid@100=10, bid@99=20 → 30; ask@101=15 → 15
    // imb = (30-15)/(30+15) = 15/45 = 1/3
    double expected = 15.0 / 45.0;
    EXPECT_NEAR(imbalanceBand(s, 99.0, 101.0, 0.0), expected, 1e-10);
}

// ---------------------------------------------------------------
// meta
// ---------------------------------------------------------------

TEST(ObMaterializerTest, MetaSeq) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "seq"), 42.0);
}

TEST(ObMaterializerTest, MetaEpoch) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "epoch"), 7.0);
}

TEST(ObMaterializerTest, MetaIsStale) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "is_stale"), 0.0);
    s.meta.stale = true;
    EXPECT_DOUBLE_EQ(meta(s, "is_stale"), 1.0);
}

TEST(ObMaterializerTest, MetaLevelsBid) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "levels.bid"), 3.0);
}

TEST(ObMaterializerTest, MetaLevelsAsk) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "levels.ask"), 3.0);
}

TEST(ObMaterializerTest, MetaLastChangeMs) {
    auto s = makeTestSnap();
    EXPECT_DOUBLE_EQ(meta(s, "last_change_ms"), 1700000000000.0);
}

TEST(ObMaterializerTest, MetaUnknownFieldReturnsNaN) {
    auto s = makeTestSnap();
    EXPECT_TRUE(std::isnan(meta(s, "garbage")));
}

// ---------------------------------------------------------------
// eval() dispatch
// ---------------------------------------------------------------

TEST(ObMaterializerTest, EvalDispatchBestPrice) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::Best; k.bestSide = Side::Bid; k.bestAttr = Target::Price;
    EXPECT_DOUBLE_EQ(eval(s, k), 100.0);
}

TEST(ObMaterializerTest, EvalDispatchBestSize) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::Best; k.bestSide = Side::Bid; k.bestAttr = Target::Size;
    EXPECT_DOUBLE_EQ(eval(s, k), 10.0);
}

TEST(ObMaterializerTest, EvalDispatchSpread) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::Spread;
    EXPECT_DOUBLE_EQ(eval(s, k), 1.0);
}

TEST(ObMaterializerTest, EvalDispatchCum) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::Cum; k.cumSide = Side::Bid; k.cumN = 2; k.cumTarget = Target::Size;
    EXPECT_DOUBLE_EQ(eval(s, k), 30.0);
}

TEST(ObMaterializerTest, EvalDispatchVwapLevels) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::VWAP; k.vwapSide = Side::Bid;
    k.vwapByLevels = true; k.vwapLv = {1, 2};
    double expected = (100.0*10.0 + 99.0*20.0) / 30.0;
    EXPECT_NEAR(eval(s, k), expected, 1e-10);
}

TEST(ObMaterializerTest, EvalDispatchVwapPrice) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::VWAP; k.vwapSide = Side::Ask;
    k.vwapByLevels = false; k.vwapP1 = 101.0; k.vwapP2 = 102.0;
    double expected = (101.0*15.0 + 102.0*25.0) / 40.0;
    EXPECT_NEAR(eval(s, k), expected, 1e-10);
}

TEST(ObMaterializerTest, EvalDispatchMeta) {
    auto s = makeTestSnap();
    ObKey k; k.metric = Metric::Meta; k.metaField = "seq";
    EXPECT_DOUBLE_EQ(eval(s, k), 42.0);
}
