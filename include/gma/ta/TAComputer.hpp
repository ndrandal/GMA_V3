#pragma once
#include <deque>
#include <unordered_map>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>
#include <limits>
#include "gma/ta/Indicators.hpp"
#include "gma/ta/AtomicNames.hpp"

namespace gma::ta {

struct TAConfig {
  size_t historyMax = 4096;
  std::vector<int> smaPeriods  = {5, 10, 20, 50};
  std::vector<int> emaPeriods  = {10, 20};
  std::vector<int> vwapPeriods = {10, 50};
  std::vector<int> medPeriods  = {5, 21};
  std::vector<int> minPeriods  = {10};
  std::vector<int> maxPeriods  = {10};
  std::vector<int> stdPeriods  = {20};
  int rsiPeriod = 14;
  bool writeLast = true; // write px.last too
};

// Store write callback: symbol, key, value, ts_ms
using StoreWriteFn = std::function<void(const std::string&, const std::string&, double, int64_t)>;
// Optional notify callback (e.g., dispatcher push)
using NotifyFn     = std::function<void(const std::string&, const std::string&)>;

class TAComputer {
public:
  TAComputer(TAConfig cfg, StoreWriteFn writer, NotifyFn notify = nullptr)
  : cfg_(std::move(cfg)), write_(std::move(writer)), notify_(std::move(notify)) {}

  // Call this for each trade/tick you consider "price/size" for indicators.
  // If size/volume is unknown, pass 1.0 so VWAP degenerates to arithmetic mean.
  void onTick(const std::string& symbol, double lastPrice, double sizeOrVol, int64_t ts_ms);

  // For quotes-only streams, you can still call onTick with mid or last traded.
  // Expose a way to prime/reset per symbol (optional).
  void reset(const std::string& symbol);

private:
  struct EmaState { double val = NaN(); bool init = false; };
  struct SymState {
    std::deque<double> px;   // recent prices
    std::deque<double> vol;  // recent sizes/volumes
    std::unordered_map<int, EmaState> ema; // period -> EMA state
    RsiState rsi;
    size_t rsiCount = 0;     // to know when to flip rsi.init=true
  };

  SymState& sym(const std::string& s);

  void bound(std::deque<double>& dq);
  void writeKV(const std::string& sym, const std::string& key, double v, int64_t ts);

private:
  TAConfig cfg_;
  StoreWriteFn write_;
  NotifyFn notify_;

  std::unordered_map<std::string, SymState> states_;
};

} // namespace gma::ta
