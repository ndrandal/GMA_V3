#include "gma/ta/TAComputer.hpp"
#include <chrono>

namespace gma::ta {

SymState& TAComputer::sym(const std::string& s) {
  return states_[s]; // default constructed
}

void TAComputer::bound(std::deque<double>& dq) {
  while (dq.size() > cfg_.historyMax) dq.pop_front();
}

void TAComputer::writeKV(const std::string& sym, const std::string& key, double v, int64_t ts){
  write_(sym, key, v, ts);
  if (notify_) notify_(sym, key);
}

void TAComputer::reset(const std::string& s){
  states_.erase(s);
}

void TAComputer::onTick(const std::string& symbol, double lastPrice, double sizeOrVol, int64_t ts_ms) {
  auto& S = sym(symbol);

  // 1) Append price and volume, keep bounds
  S.px.push_back(lastPrice);
  S.vol.push_back(sizeOrVol <= 0.0 ? 1.0 : sizeOrVol);
  bound(S.px); bound(S.vol);

  if (cfg_.writeLast) {
    writeKV(symbol, key_px_last(), lastPrice, ts_ms);
  }

  // 2) SMA / Median / Min / Max / StdDev over configured periods
  for (int n : cfg_.smaPeriods) {
    double v = sma_lastN(S.px, n);
    if (isFinite(v)) writeKV(symbol, key_px_sma(n), v, ts_ms);
  }
  for (int n : cfg_.medPeriods) {
    double v = median_lastN(S.px, n);
    if (isFinite(v)) writeKV(symbol, key_px_med(n), v, ts_ms);
  }
  for (int n : cfg_.minPeriods) {
    double v = min_lastN(S.px, n);
    if (isFinite(v)) writeKV(symbol, key_px_min(n), v, ts_ms);
  }
  for (int n : cfg_.maxPeriods) {
    double v = max_lastN(S.px, n);
    if (isFinite(v)) writeKV(symbol, key_px_max(n), v, ts_ms);
  }
  for (int n : cfg_.stdPeriods) {
    double v = stddev_lastN(S.px, n);
    if (isFinite(v)) writeKV(symbol, key_px_std(n), v, ts_ms);
  }

  // 3) EMA (period-specific state)
  for (int n : cfg_.emaPeriods) {
    auto& e = S.ema[n];
    double next = ema_next(e.val, lastPrice, S.px, size_t(n));
    if (isFinite(next)) {
      e.val = next;
      e.init = true;
      writeKV(symbol, key_px_ema(n), next, ts_ms);
    }
  }

  // 4) VWAP over last N trades
  for (int n : cfg_.vwapPeriods) {
    double v = vwap_lastN(S.px, S.vol, n);
    if (isFinite(v)) writeKV(symbol, key_px_vwap(n), v, ts_ms);
  }

  // 5) RSI (Wilder)
  if (cfg_.rsiPeriod > 0) {
    // Count until we reach period samples, then flip init
    if (S.rsiCount < size_t(cfg_.rsiPeriod)) {
      double r = rsi_update(S.rsi, lastPrice, size_t(cfg_.rsiPeriod));
      S.rsiCount++;
      if (S.rsiCount >= size_t(cfg_.rsiPeriod)) S.rsi.init = true;
      if (isFinite(r)) writeKV(symbol, key_px_rsi(cfg_.rsiPeriod), r, ts_ms);
    } else {
      double r = rsi_update(S.rsi, lastPrice, size_t(cfg_.rsiPeriod));
      if (isFinite(r)) writeKV(symbol, key_px_rsi(cfg_.rsiPeriod), r, ts_ms);
    }
  }
}

} // namespace gma::ta
