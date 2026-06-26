// ENC-802 (M16): numeric-correctness tests for the LIVE technical-analysis
// indicators.
//
// The rigorous tests in tests/core/IndicatorsTest.cpp validate the library
// header connectors/.../ta/Indicators.hpp (Wilder RSI, ema_next, sma_lastN) —
// but that header is dead code in production. The indicators that actually run
// are the inline reimplementations inside MarketTA.cpp, reached through
// computeAllAtomicValues() (which MarketTickComputer::compute() calls on every
// tick). tests/core/AtomicFunctionsTest.cpp only checks has_value() on most of
// them, so a wrong formula would still pass.
//
// These tests drive the PRODUCTION path — computeAllAtomicValues() directly and
// MarketTickComputer::compute() end-to-end — and assert exact expected values
// computed by hand against the production formulas AS THEY ARE (e.g. production
// RSI is a simple-average, NOT Wilder; all-flat RSI degenerates to 0, see L5).
// A wrong production formula now fails a test.

#include "gma/MarketTA.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace gma;

namespace {

double getD(const AtomicStore& store, const std::string& sym, const std::string& key) {
  auto v = store.get(sym, key);
  EXPECT_TRUE(v.has_value()) << "missing " << sym << "::" << key;
  if (!v.has_value()) return std::nan("");
  return std::get<double>(*v);
}

// Build a price-only history (volume held at 1.0 so VWAP == mean and OBV stays
// tractable; the indicators under test depend only on price).
std::vector<TickEntry> series(const std::vector<double>& prices) {
  std::vector<TickEntry> h;
  h.reserve(prices.size());
  for (double p : prices) h.push_back(TickEntry{p, 1.0});
  return h;
}

} // namespace

// ---------------------------------------------------------------------------
// SMA — last-N simple mean.  prices [10,20,30,40,50], period 3 -> (30+40+50)/3.
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, SmaExactLastN) {
  util::Config cfg;
  cfg.taSMA = {3};
  AtomicStore store;
  computeAllAtomicValues("SMA", series({10, 20, 30, 40, 50}), store, cfg);

  EXPECT_DOUBLE_EQ(getD(store, "SMA", "sma_3"), (30.0 + 40.0 + 50.0) / 3.0); // 40
  // sma_5 not configured -> absent.
  EXPECT_FALSE(store.get("SMA", "sma_5").has_value());
}

// ---------------------------------------------------------------------------
// EMA — production seeds with hist[0] and folds across the FULL history with
// k = 2/(period+1). For [10,20,30,40,50], period 3 (k=0.5) this is exactly:
//   10 -> 15 -> 22.5 -> 31.25 -> 40.625
// (all powers of two, so representable exactly in double).
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, EmaExactFullHistorySeed) {
  util::Config cfg;
  cfg.taEMA = {3};
  AtomicStore store;
  computeAllAtomicValues("EMA", series({10, 20, 30, 40, 50}), store, cfg);

  EXPECT_DOUBLE_EQ(getD(store, "EMA", "ema_3"), 40.625);
}

// ---------------------------------------------------------------------------
// RSI — production is a SIMPLE average of gains/losses over the last `period`
// deltas (NOT Wilder). Symmetric up/down moves give avgGain==avgLoss -> rs==1
// -> RSI==50 exactly. prices [100,102,100,102,100], period 4:
//   deltas = +2,-2,+2,-2 ; gain=4 loss=4 ; rs=1 ; rsi=100-100/2=50.
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, RsiSimpleAverageBalancedIsFifty) {
  util::Config cfg;
  cfg.taRSI = 4;
  AtomicStore store;
  computeAllAtomicValues("RSI", series({100, 102, 100, 102, 100}), store, cfg);

  EXPECT_DOUBLE_EQ(getD(store, "RSI", "rsi_4"), 50.0);
}

// ---------------------------------------------------------------------------
// RSI degenerate all-flat case (L5/ENC-804): no gains, no losses -> RSI is
// undefined (0/0). Production reports the neutral midpoint 50 rather than a
// misleading 0 (the pre-L5 `rs = 0/EPSILON` path). This pins that fix.
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, RsiAllFlatIsNeutral) {
  util::Config cfg;
  cfg.taRSI = 4;
  AtomicStore store;
  computeAllAtomicValues("FLAT", series({50, 50, 50, 50, 50}), store, cfg);

  EXPECT_DOUBLE_EQ(getD(store, "FLAT", "rsi_4"), 50.0);
}

// ---------------------------------------------------------------------------
// Bollinger Bands — SMA(n) +/- k * population-stddev(n). prices last 4 of
// [10,20,30,40,50] = [20,30,40,50]: sma=35, variance = mean(15^2,5^2,5^2,15^2)
// = 500/4 = 125, stddev = sqrt(125), k=2. Upper/lower are symmetric about 35.
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, BollingerBandsExact) {
  util::Config cfg;
  cfg.taBBands_n   = 4;
  cfg.taBBands_stdK = 2.0;
  AtomicStore store;
  computeAllAtomicValues("BB", series({10, 20, 30, 40, 50}), store, cfg);

  const double sma = 35.0;
  const double stddev = std::sqrt(125.0);
  EXPECT_NEAR(getD(store, "BB", "bollinger_upper"), sma + 2.0 * stddev, 1e-9);
  EXPECT_NEAR(getD(store, "BB", "bollinger_lower"), sma - 2.0 * stddev, 1e-9);
  // Midpoint must be the SMA exactly (catches an asymmetric-band regression).
  EXPECT_NEAR((getD(store, "BB", "bollinger_upper") +
               getD(store, "BB", "bollinger_lower")) / 2.0, sma, 1e-9);
}

// ---------------------------------------------------------------------------
// MACD — exercises the full production series path (n >= slow). Using small
// periods fast=2 (k=2/3), slow=3 (k=1/2), signal=2 over prices [1,2,3,4,5],
// the exact rational results (verified by hand) are:
//   macd_line      = 575/1296  = 0.443672839506...
//   macd_signal    = 415/972   = 0.426954732510...
//   macd_histogram = 65/3888   = 0.016718106996...
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, MacdSeriesPathExact) {
  util::Config cfg;
  cfg.taMACD_fast   = 2;
  cfg.taMACD_slow   = 3;
  cfg.taMACD_signal = 2;
  AtomicStore store;
  computeAllAtomicValues("MACD", series({1, 2, 3, 4, 5}), store, cfg);

  EXPECT_NEAR(getD(store, "MACD", "macd_line"),      575.0 / 1296.0, 1e-9);
  EXPECT_NEAR(getD(store, "MACD", "macd_signal"),    415.0 / 972.0,  1e-9);
  EXPECT_NEAR(getD(store, "MACD", "macd_histogram"), 65.0 / 3888.0,  1e-9);
  // Histogram identity must hold regardless of formula tweaks.
  EXPECT_NEAR(getD(store, "MACD", "macd_histogram"),
              getD(store, "MACD", "macd_line") - getD(store, "MACD", "macd_signal"),
              1e-12);
}

// ---------------------------------------------------------------------------
// VWAP / mean — VWAP is volume-weighted; with non-uniform volumes it must
// differ from the plain mean. prices [1,3,2] volumes [10,20,5]:
//   mean = (1+3+2)/3 = 2 ; vwap = (1*10+3*20+2*5)/35 = 80/35.
// ---------------------------------------------------------------------------
TEST(MarketTANumericTest, VwapVolumeWeightedExact) {
  AtomicStore store;
  std::vector<TickEntry> hist = {{1.0, 10.0}, {3.0, 20.0}, {2.0, 5.0}};
  computeAllAtomicValues("VW", hist, store);

  EXPECT_DOUBLE_EQ(getD(store, "VW", "mean"), 2.0);
  EXPECT_NEAR(getD(store, "VW", "vwap"), 80.0 / 35.0, 1e-12);
}

// ---------------------------------------------------------------------------
// End-to-end through the real MarketTickComputer: feed JSON ticks the way the
// dispatcher does, and confirm the computer extracts price, builds history, and
// runs computeAllAtomicValues — asserting an exact SMA and the latest price.
// This proves the production tick path (not just the bare function) is correct.
// ---------------------------------------------------------------------------
namespace {
Event makeTick(const std::string& sym, double lastPrice) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  doc->AddMember("lastPrice", rapidjson::Value(lastPrice), doc->GetAllocator());
  Event e;
  e.type = "tick";
  e.symbol = sym;
  e.payload = std::move(doc);
  return e;
}
} // namespace

TEST(MarketTANumericTest, MarketTickComputerEndToEndSma) {
  registerBuiltinFunctions();
  rt::ThreadPool pool(1);
  AtomicStore store;
  util::Config cfg;
  cfg.taSMA = {5};

  MarketTickComputer computer(cfg);   // default NASDAQ-style field map
  Dispatcher dispatcher(&pool, &store, cfg);
  engine::ComputeContext ctx{&store, &dispatcher, &pool};

  for (double p : {10.0, 20.0, 30.0, 40.0, 50.0}) {
    Event ev = makeTick("E2E", p);
    computer.compute(ev, ctx);
  }
  pool.shutdown();

  EXPECT_DOUBLE_EQ(getD(store, "E2E", "lastPrice"), 50.0);
  EXPECT_DOUBLE_EQ(getD(store, "E2E", "sma_5"), 30.0); // mean of 10..50
}
