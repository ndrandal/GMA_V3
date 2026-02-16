#include "gma/ob/ObProvider.hpp"
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <limits>

using namespace gma::ob;

static Snapshot makeProviderSnap() {
    Snapshot s;
    // Bids: 100@10, 99@20, 98@30
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
    s.meta.seq = 1;
    s.meta.bidLevels = 3;
    s.meta.askLevels = 3;
    return s;
}

static std::shared_ptr<FunctionalSnapshotSource> makeSource() {
    return std::make_shared<FunctionalSnapshotSource>(
        [](const std::string& /*sym*/, size_t /*n*/, Mode /*m*/,
           std::optional<std::pair<double,double>> /*band*/) {
            return makeProviderSnap();
        },
        [](const std::string& /*sym*/) { return 0.01; }
    );
}

TEST(ObProviderTest, GetSpreadFastPath) {
    Provider p(makeSource(), 5, 5);
    double v = p.get("SYM", "ob.spread");
    EXPECT_DOUBLE_EQ(v, 1.0); // 101 - 100
}

TEST(ObProviderTest, GetLevelBidPrice) {
    Provider p(makeSource(), 5, 5);
    double v = p.get("SYM", "ob.level.bid.1.price");
    EXPECT_DOUBLE_EQ(v, 100.0);
}

TEST(ObProviderTest, GetCumBidLevelsSize) {
    Provider p(makeSource(), 5, 5);
    double v = p.get("SYM", "ob.cum.bid.levels.3.size");
    // 10 + 20 + 30 = 60
    EXPECT_DOUBLE_EQ(v, 60.0);
}

TEST(ObProviderTest, GetImbalanceLevels) {
    Provider p(makeSource(), 5, 5);
    double v = p.get("SYM", "ob.imbalance.levels.1-3");
    // bid=10+20+30=60, ask=15+25+35=75, imb=(60-75)/(60+75) = -15/135
    double expected = -15.0 / 135.0;
    EXPECT_NEAR(v, expected, 1e-10);
}

TEST(ObProviderTest, GetUnknownKeyReturnsNaN) {
    Provider p(makeSource(), 5, 5);
    double v = p.get("SYM", "ob.garbage.nonsense");
    EXPECT_TRUE(std::isnan(v));
}

TEST(ObProviderTest, GetWithNullSourceReturnsNaN) {
    Provider p(nullptr, 5, 5);
    double v = p.get("SYM", "ob.spread");
    EXPECT_TRUE(std::isnan(v));
}
