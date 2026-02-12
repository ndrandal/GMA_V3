#include "gma/ta/Indicators.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <deque>

using namespace gma::ta;

// --- SMA ---
TEST(IndicatorsTest, SmaBasic) {
    std::deque<double> xs = {1.0, 2.0, 3.0, 4.0, 5.0};
    EXPECT_DOUBLE_EQ(sma_lastN(xs, 5), 3.0);
    EXPECT_DOUBLE_EQ(sma_lastN(xs, 3), 4.0); // last 3: 3,4,5
}

TEST(IndicatorsTest, SmaReturnsNanWhenInsufficient) {
    std::deque<double> xs = {1.0, 2.0};
    EXPECT_TRUE(std::isnan(sma_lastN(xs, 5)));
}

// --- EMA ---
TEST(IndicatorsTest, EmaInitializesWithSma) {
    std::deque<double> xs = {1.0, 2.0, 3.0};
    double result = ema_next(NaN(), 3.0, xs, 3);
    EXPECT_DOUBLE_EQ(result, 2.0); // SMA of {1,2,3} = 2
}

TEST(IndicatorsTest, EmaIncremental) {
    std::deque<double> xs = {1.0, 2.0, 3.0};
    double prev = ema_next(NaN(), 3.0, xs, 3);
    double next = ema_next(prev, 4.0, xs, 3);
    // alpha = 2/(3+1) = 0.5; EMA = 0.5*4 + 0.5*2 = 3.0
    EXPECT_DOUBLE_EQ(next, 3.0);
}

// --- RSI ---
TEST(IndicatorsTest, RsiReturnsNanDuringInit) {
    RsiState st;
    // First call just seeds lastPx.
    EXPECT_TRUE(std::isnan(rsi_update(st, 100.0, 14)));
    // Subsequent calls during init also return NaN.
    for (int i = 1; i <= 5; ++i) {
        EXPECT_TRUE(std::isnan(rsi_update(st, 100.0 + i, 14)));
    }
}

TEST(IndicatorsTest, RsiProducesValueAfterPeriodSamples) {
    RsiState st;
    const size_t period = 14;

    // First call seeds lastPx, returns NaN.
    rsi_update(st, 100.0, period);

    // Feed 'period' price changes to complete init.
    for (size_t i = 1; i <= period; ++i) {
        double px = 100.0 + static_cast<double>(i) * 0.5;
        double result = rsi_update(st, px, period);
        if (i < period) {
            EXPECT_TRUE(std::isnan(result)) << "Should be NaN at step " << i;
        } else {
            // After 'period' samples, init transitions and we get a value.
            EXPECT_TRUE(st.init) << "init should be true after period samples";
            EXPECT_FALSE(std::isnan(result)) << "Should produce RSI after init";
            EXPECT_GE(result, 0.0);
            EXPECT_LE(result, 100.0);
        }
    }

    // After init, subsequent calls should produce values.
    double v = rsi_update(st, 110.0, period);
    EXPECT_FALSE(std::isnan(v));
    EXPECT_GE(v, 0.0);
    EXPECT_LE(v, 100.0);
}

TEST(IndicatorsTest, RsiAllGainsReturns100) {
    RsiState st;
    const size_t period = 5;
    rsi_update(st, 100.0, period);

    // All gains: avgLoss should approach 0, RSI should approach 100.
    double result = NaN();
    for (size_t i = 1; i <= period + 5; ++i) {
        result = rsi_update(st, 100.0 + static_cast<double>(i), period);
    }
    EXPECT_GT(result, 95.0); // Should be very close to 100
}

// --- Median ---
TEST(IndicatorsTest, MedianOddCount) {
    std::deque<double> xs = {5.0, 1.0, 3.0, 2.0, 4.0};
    EXPECT_DOUBLE_EQ(median_lastN(xs, 5), 3.0);
}

TEST(IndicatorsTest, MedianEvenCount) {
    std::deque<double> xs = {1.0, 3.0, 2.0, 4.0};
    EXPECT_DOUBLE_EQ(median_lastN(xs, 4), 2.5);
}

// --- StdDev ---
TEST(IndicatorsTest, StddevUniform) {
    std::deque<double> xs = {5.0, 5.0, 5.0, 5.0};
    EXPECT_DOUBLE_EQ(stddev_lastN(xs, 4), 0.0);
}
