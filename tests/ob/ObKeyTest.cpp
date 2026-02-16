#include "gma/ob/ObKey.hpp"
#include <gtest/gtest.h>
#include <string>

using namespace gma::ob;

// ---------------------------------------------------------------
// isObKey
// ---------------------------------------------------------------

TEST(ObKeyTest, IsObKeyValidPrefixes) {
    EXPECT_TRUE(isObKey("ob.spread"));
    EXPECT_TRUE(isObKey("ob.mid"));
    EXPECT_TRUE(isObKey("ob.best.bid.price"));
    EXPECT_TRUE(isObKey("ob.level.bid.1.price"));
}

TEST(ObKeyTest, IsObKeyInvalidPrefixes) {
    EXPECT_FALSE(isObKey("ta.sma_5"));
    EXPECT_FALSE(isObKey("spread"));
    EXPECT_FALSE(isObKey(""));
    EXPECT_FALSE(isObKey("OB.spread"));
}

// ---------------------------------------------------------------
// parseObKey: simple metrics
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseSpread) {
    auto k = parseObKey("ob.spread");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Spread);
}

TEST(ObKeyTest, ParseMid) {
    auto k = parseObKey("ob.mid");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Mid);
}

// ---------------------------------------------------------------
// parseObKey: best
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseBestBidPrice) {
    auto k = parseObKey("ob.best.bid.price");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Best);
    EXPECT_EQ(k->bestSide, Side::Bid);
    EXPECT_EQ(k->bestAttr, Target::Price);
}

TEST(ObKeyTest, ParseBestAskSize) {
    auto k = parseObKey("ob.best.ask.size");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Best);
    EXPECT_EQ(k->bestSide, Side::Ask);
    EXPECT_EQ(k->bestAttr, Target::Size);
}

// ---------------------------------------------------------------
// parseObKey: level
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseLevelBidNPrice) {
    auto k = parseObKey("ob.level.bid.3.price");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::LevelIdx);
    EXPECT_EQ(k->levelIdx.side, Side::Bid);
    EXPECT_EQ(k->levelIdx.n, 3);
    EXPECT_EQ(k->levelIdx.attr, Target::Price);
}

TEST(ObKeyTest, ParseAtBidPriceSize) {
    // Note: decimal prices containing '.' are ambiguous with the key separator,
    // so only integer prices work in the dot-delimited format.
    auto k = parseObKey("ob.at.bid.100.size");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::LevelPx);
    EXPECT_EQ(k->levelPx.side, Side::Bid);
    EXPECT_DOUBLE_EQ(k->levelPx.px, 100.0);
    EXPECT_EQ(k->levelPx.attr, Target::Size);
}

// ---------------------------------------------------------------
// parseObKey: cum
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseCumBidLevelsNSize) {
    auto k = parseObKey("ob.cum.bid.levels.5.size");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Cum);
    EXPECT_EQ(k->cumSide, Side::Bid);
    EXPECT_EQ(k->cumN, 5);
    EXPECT_EQ(k->cumTarget, Target::Size);
}

// ---------------------------------------------------------------
// parseObKey: vwap
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseVwapBidLevelsN) {
    auto k = parseObKey("ob.vwap.bid.levels.5");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::VWAP);
    EXPECT_EQ(k->vwapSide, Side::Bid);
    EXPECT_TRUE(k->vwapByLevels);
    EXPECT_EQ(k->vwapLv.a, 1);
    EXPECT_EQ(k->vwapLv.b, 5);
}

TEST(ObKeyTest, ParseVwapBidLevelsAB) {
    auto k = parseObKey("ob.vwap.bid.levels.2-8");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::VWAP);
    EXPECT_EQ(k->vwapSide, Side::Bid);
    EXPECT_TRUE(k->vwapByLevels);
    EXPECT_EQ(k->vwapLv.a, 2);
    EXPECT_EQ(k->vwapLv.b, 8);
}

TEST(ObKeyTest, ParseVwapAskPriceBand) {
    auto k = parseObKey("ob.vwap.ask.price.100-200");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::VWAP);
    EXPECT_EQ(k->vwapSide, Side::Ask);
    EXPECT_FALSE(k->vwapByLevels);
    EXPECT_DOUBLE_EQ(k->vwapP1, 100.0);
    EXPECT_DOUBLE_EQ(k->vwapP2, 200.0);
}

// ---------------------------------------------------------------
// parseObKey: imbalance
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseImbalanceLevelsN) {
    auto k = parseObKey("ob.imbalance.levels.5");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Imbalance);
    EXPECT_TRUE(k->imbByLevels);
    EXPECT_EQ(k->imbLv.a, 1);
    EXPECT_EQ(k->imbLv.b, 5);
}

TEST(ObKeyTest, ParseImbalanceLevelsAB) {
    auto k = parseObKey("ob.imbalance.levels.2-7");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Imbalance);
    EXPECT_TRUE(k->imbByLevels);
    EXPECT_EQ(k->imbLv.a, 2);
    EXPECT_EQ(k->imbLv.b, 7);
}

TEST(ObKeyTest, ParseImbalancePriceBand) {
    auto k = parseObKey("ob.imbalance.price.90-110");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Imbalance);
    EXPECT_FALSE(k->imbByLevels);
    EXPECT_DOUBLE_EQ(k->imbP1, 90.0);
    EXPECT_DOUBLE_EQ(k->imbP2, 110.0);
}

// ---------------------------------------------------------------
// parseObKey: range
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseRangeIdxSumSize) {
    auto k = parseObKey("ob.range.bid.levels.1-5.sum.size");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::RangeIdx);
    EXPECT_EQ(k->rangeIdx.side, Side::Bid);
    EXPECT_EQ(k->rangeIdx.lv.a, 1);
    EXPECT_EQ(k->rangeIdx.lv.b, 5);
    EXPECT_EQ(k->rangeIdx.reduce, Reduce::Sum);
    EXPECT_EQ(k->rangeIdx.target, Target::Size);
}

TEST(ObKeyTest, ParseRangeIdxCount) {
    auto k = parseObKey("ob.range.ask.levels.1-3.count");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::RangeIdx);
    EXPECT_EQ(k->rangeIdx.side, Side::Ask);
    EXPECT_EQ(k->rangeIdx.lv.a, 1);
    EXPECT_EQ(k->rangeIdx.lv.b, 3);
    EXPECT_EQ(k->rangeIdx.reduce, Reduce::Count);
}

TEST(ObKeyTest, ParseRangePxAvgPrice) {
    auto k = parseObKey("ob.range.bid.price.50-150.avg.price");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::RangePx);
    EXPECT_EQ(k->rangePx.side, Side::Bid);
    EXPECT_DOUBLE_EQ(k->rangePx.p1, 50.0);
    EXPECT_DOUBLE_EQ(k->rangePx.p2, 150.0);
    EXPECT_EQ(k->rangePx.reduce, Reduce::Avg);
    EXPECT_EQ(k->rangePx.target, Target::Price);
}

// ---------------------------------------------------------------
// parseObKey: meta
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseMetaSeq) {
    auto k = parseObKey("ob.meta.seq");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Meta);
    EXPECT_EQ(k->metaField, "seq");
}

TEST(ObKeyTest, ParseMetaLevelsBid) {
    auto k = parseObKey("ob.meta.levels.bid");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Meta);
    EXPECT_EQ(k->metaField, "levels.bid");
}

// ---------------------------------------------------------------
// parseObKey: mode suffix
// ---------------------------------------------------------------

TEST(ObKeyTest, ParseModePerSuffix) {
    auto k = parseObKey("ob.spread.per");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Spread);
    EXPECT_EQ(k->mode, Mode::Per);
}

TEST(ObKeyTest, ParseModeAggSuffix) {
    auto k = parseObKey("ob.mid.agg");
    ASSERT_TRUE(k.has_value());
    EXPECT_EQ(k->metric, Metric::Mid);
    EXPECT_EQ(k->mode, Mode::Agg);
}

// ---------------------------------------------------------------
// parseObKey: edge cases
// ---------------------------------------------------------------

TEST(ObKeyTest, InvalidKeyReturnsNullopt) {
    EXPECT_FALSE(parseObKey("ob.nonsense").has_value());
    EXPECT_FALSE(parseObKey("").has_value());
    EXPECT_FALSE(parseObKey("ta.sma_5").has_value());
    EXPECT_FALSE(parseObKey("ob.best.bid.notional").has_value()); // only price/size allowed
    EXPECT_FALSE(parseObKey("ob.level.bid.0.price").has_value()); // n < 1
}

// ---------------------------------------------------------------
// formatObKey: round-trip for each metric type
// ---------------------------------------------------------------

TEST(ObKeyTest, FormatRoundTripSpread) {
    auto k = parseObKey("ob.spread");
    ASSERT_TRUE(k.has_value());
    auto formatted = formatObKey(*k);
    EXPECT_EQ(formatted, "ob.spread");
}

TEST(ObKeyTest, FormatRoundTripBest) {
    auto k = parseObKey("ob.best.bid.price");
    ASSERT_TRUE(k.has_value());
    auto formatted = formatObKey(*k);
    auto k2 = parseObKey(formatted);
    ASSERT_TRUE(k2.has_value());
    EXPECT_EQ(k2->metric, Metric::Best);
    EXPECT_EQ(k2->bestSide, Side::Bid);
    EXPECT_EQ(k2->bestAttr, Target::Price);
}

TEST(ObKeyTest, FormatRoundTripVwapLevels) {
    ObKey k;
    k.metric = Metric::VWAP;
    k.vwapSide = Side::Bid;
    k.vwapByLevels = true;
    k.vwapLv = {2, 8};
    auto formatted = formatObKey(k);
    auto k2 = parseObKey(formatted);
    ASSERT_TRUE(k2.has_value()) << "Failed to parse: " << formatted;
    EXPECT_EQ(k2->metric, Metric::VWAP);
    EXPECT_TRUE(k2->vwapByLevels);
    EXPECT_EQ(k2->vwapLv.a, 2);
    EXPECT_EQ(k2->vwapLv.b, 8);
}

TEST(ObKeyTest, FormatRoundTripRangeIdx) {
    ObKey k;
    k.metric = Metric::RangeIdx;
    k.rangeIdx.side = Side::Bid;
    k.rangeIdx.lv = {1, 5};
    k.rangeIdx.reduce = Reduce::Sum;
    k.rangeIdx.target = Target::Size;
    auto formatted = formatObKey(k);
    auto k2 = parseObKey(formatted);
    ASSERT_TRUE(k2.has_value()) << "Failed to parse: " << formatted;
    EXPECT_EQ(k2->metric, Metric::RangeIdx);
    EXPECT_EQ(k2->rangeIdx.lv.a, 1);
    EXPECT_EQ(k2->rangeIdx.lv.b, 5);
    EXPECT_EQ(k2->rangeIdx.reduce, Reduce::Sum);
    EXPECT_EQ(k2->rangeIdx.target, Target::Size);
}

TEST(ObKeyTest, FormatRoundTripImbalanceLevels) {
    ObKey k;
    k.metric = Metric::Imbalance;
    k.imbByLevels = true;
    k.imbLv = {2, 7};
    auto formatted = formatObKey(k);
    auto k2 = parseObKey(formatted);
    ASSERT_TRUE(k2.has_value()) << "Failed to parse: " << formatted;
    EXPECT_EQ(k2->metric, Metric::Imbalance);
    EXPECT_TRUE(k2->imbByLevels);
    EXPECT_EQ(k2->imbLv.a, 2);
    EXPECT_EQ(k2->imbLv.b, 7);
}

TEST(ObKeyTest, FormatRoundTripMeta) {
    auto k = parseObKey("ob.meta.levels.ask");
    ASSERT_TRUE(k.has_value());
    auto formatted = formatObKey(*k);
    EXPECT_EQ(formatted, "ob.meta.levels.ask");
}

TEST(ObKeyTest, FormatRoundTripModeAgg) {
    auto k = parseObKey("ob.spread.agg");
    ASSERT_TRUE(k.has_value());
    auto formatted = formatObKey(*k);
    EXPECT_EQ(formatted, "ob.spread.agg");
}
