#include "gma/AtomicFunctions.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/util/Config.hpp"
#include <gtest/gtest.h>
#include <string>
#include <cmath>

using namespace gma;

// Helpers to retrieve typed values from the store (guard against nullopt UB)
static double getDouble(const AtomicStore& store, const std::string& sym, const std::string& key) {
    auto val = store.get(sym, key);
    EXPECT_TRUE(val.has_value()) << "Missing double for " << sym << "::" << key;
    if (!val.has_value()) return 0.0;
    return std::get<double>(*val);
}

TEST(AtomicFunctionsTest, BasicPriceMetrics) {
    SymbolHistory hist = {{1.0,10.0}, {3.0,20.0}, {2.0,5.0}};
    AtomicStore store;
    const std::string sym = "TEST";
    computeAllAtomicValues(sym, hist, store);

    EXPECT_DOUBLE_EQ(getDouble(store, sym, "openPrice"), 1.0);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "lastPrice"), 2.0);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "highPrice"), 3.0);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "lowPrice"), 1.0);
    // prevClose = second-to-last price
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "prevClose"), 3.0);
}

TEST(AtomicFunctionsTest, MeanAndMedianAndVwap) {
    SymbolHistory hist = {{1.0,10.0}, {3.0,20.0}, {2.0,5.0}};
    AtomicStore store;
    const std::string sym = "STATS";
    computeAllAtomicValues(sym, hist, store);

    // mean = (1+3+2)/3 = 2
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "mean"), 2.0);
    // median = 2.0
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "median"), 2.0);
    // VWAP = (1*10 + 3*20 + 2*5) / 35 = 80/35
    EXPECT_NEAR(getDouble(store, sym, "vwap"), 80.0/35.0, 1e-10);
}

// Helper to compute sum of consecutive ints
static double sumRange(int start, int end) {
    int count = end - start + 1;
    return (start + end) * count / 2.0;
}

TEST(AtomicFunctionsTest, TechnicalIndicatorsPresence) {
    // Use history >= 20 entries to ensure all indicators are generated
    SymbolHistory hist;
    for (int i = 1; i <= 25; ++i) hist.push_back({static_cast<double>(i), static_cast<double>(2*i)});
    AtomicStore store;
    const std::string sym = "TECH";
    computeAllAtomicValues(sym, hist, store);

    // SMA/EMA keys — prices are 1..25, SMA(5) = avg(21..25), SMA(20) = avg(6..25)
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "sma_5"), (21+22+23+24+25)/5.0);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "sma_20"), sumRange(6, 25) / 20.0);
    EXPECT_TRUE(store.get(sym, "ema_12").has_value());
    EXPECT_TRUE(store.get(sym, "ema_26").has_value());
    // RSI (14)
    EXPECT_TRUE(store.get(sym, "rsi_14").has_value());
    // MACD
    EXPECT_TRUE(store.get(sym, "macd_line").has_value());
    EXPECT_TRUE(store.get(sym, "macd_signal").has_value());
    EXPECT_TRUE(store.get(sym, "macd_histogram").has_value());
    // Bollinger bands
    EXPECT_TRUE(store.get(sym, "bollinger_upper").has_value());
    EXPECT_TRUE(store.get(sym, "bollinger_lower").has_value());
    // Momentum & ROC
    EXPECT_TRUE(store.get(sym, "momentum_10").has_value());
    EXPECT_TRUE(store.get(sym, "roc_10").has_value());
    // ATR
    EXPECT_TRUE(store.get(sym, "atr_14").has_value());
    // Volume metrics
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "volume"), 2*25);
    EXPECT_TRUE(store.get(sym, "volume_avg_20").has_value());
    // OBV
    EXPECT_TRUE(store.get(sym, "obv").has_value());
    // Volatility rank
    EXPECT_TRUE(store.get(sym, "volatility_rank").has_value());
}

TEST(AtomicFunctionsTest, InsufficientHistoryTriggersPartialMetrics) {
    // Only 3 entries: no RSI, no ATR, no bollinger
    SymbolHistory hist = {{10,1}, {12,1}, {11,1}};
    AtomicStore store;
    const std::string sym = "PARTIAL";
    computeAllAtomicValues(sym, hist, store);

    // Basic always present
    EXPECT_TRUE(store.get(sym, "mean").has_value());
    EXPECT_TRUE(store.get(sym, "median").has_value());
    // RSI_14 absent
    EXPECT_FALSE(store.get(sym, "rsi_14").has_value());
    // ATR_14 absent
    EXPECT_FALSE(store.get(sym, "atr_14").has_value());
    // Bollinger absent
    EXPECT_FALSE(store.get(sym, "bollinger_upper").has_value());
}

TEST(AtomicFunctionsTest, OverwriteOnSecondCall) {
    SymbolHistory hist = {{2,1}, {4,1}};
    AtomicStore store;
    const std::string sym = "DUP";
    computeAllAtomicValues(sym, hist, store);
    // Updated history
    hist = {{10,1}, {20,1}};
    computeAllAtomicValues(sym, hist, store);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "lastPrice"), 20.0);
    EXPECT_DOUBLE_EQ(getDouble(store, sym, "mean"), 15.0);
}

TEST(AtomicFunctionsTest, CustomConfigPeriods) {
    // Use enough history to cover all custom periods
    SymbolHistory hist;
    for (int i = 1; i <= 25; ++i) hist.push_back({static_cast<double>(i), static_cast<double>(i)});

    util::Config cfg;
    cfg.taSMA = {3};
    cfg.taEMA = {5};
    cfg.taRSI = 5;

    AtomicStore store;
    const std::string sym = "CUSTOM";
    computeAllAtomicValues(sym, hist, store, cfg);

    // sma_3 should exist (custom period)
    EXPECT_TRUE(store.get(sym, "sma_3").has_value());
    // sma_5 should NOT exist (not in custom taSMA)
    EXPECT_FALSE(store.get(sym, "sma_5").has_value());
    // sma_20 should NOT exist (default period, not in custom config)
    EXPECT_FALSE(store.get(sym, "sma_20").has_value());

    // ema_5 should exist (custom period)
    EXPECT_TRUE(store.get(sym, "ema_5").has_value());
    // ema_12 should NOT exist (default period, not in custom config)
    EXPECT_FALSE(store.get(sym, "ema_12").has_value());

    // rsi_5 should exist (custom period)
    EXPECT_TRUE(store.get(sym, "rsi_5").has_value());
    // rsi_14 should NOT exist
    EXPECT_FALSE(store.get(sym, "rsi_14").has_value());
}
