#pragma once
#include <deque>
#include <vector>
#include <limits>
#include <cmath>
#include <algorithm>
#include <utility>

namespace gma::ta {

inline double NaN() { return std::numeric_limits<double>::quiet_NaN(); }
inline bool isFinite(double x) { return std::isfinite(x); }

// ------- Simple aggregations on last N -------

inline double sma_lastN(const std::deque<double>& xs, size_t N) {
  if (N==0 || xs.size() < N) return NaN();
  double s=0.0; for (size_t i=xs.size()-N; i<xs.size(); ++i) s += xs[i];
  return s / double(N);
}

inline double min_lastN(const std::deque<double>& xs, size_t N) {
  if (N==0 || xs.size() < N) return NaN();
  double m = xs[xs.size()-N];
  for (size_t i=xs.size()-N+1; i<xs.size(); ++i) m = std::min(m, xs[i]);
  return m;
}

inline double max_lastN(const std::deque<double>& xs, size_t N) {
  if (N==0 || xs.size() < N) return NaN();
  double m = xs[xs.size()-N];
  for (size_t i=xs.size()-N+1; i<xs.size(); ++i) m = std::max(m, xs[i]);
  return m;
}

inline double stddev_lastN(const std::deque<double>& xs, size_t N) {
  if (N==0 || xs.size() < N) return NaN();
  double mean = sma_lastN(xs, N);
  if (!isFinite(mean)) return NaN();
  double acc=0.0;
  for (size_t i=xs.size()-N; i<xs.size(); ++i) {
    double d = xs[i] - mean;
    acc += d*d;
  }
  return std::sqrt(acc / double(N));
}

// Median via partial copy (N small; avoids heap structures). O(N log N)
inline double median_lastN(const std::deque<double>& xs, size_t N) {
  if (N==0 || xs.size() < N) return NaN();
  std::vector<double> tmp; tmp.reserve(N);
  for (size_t i=xs.size()-N; i<xs.size(); ++i) tmp.push_back(xs[i]);
  std::sort(tmp.begin(), tmp.end());
  if (N & 1) return tmp[N/2];
  return 0.5 * (tmp[N/2 - 1] + tmp[N/2]);
}

// ------- EMA (incremental) -------
// If prevEMA is NaN, we initialize EMA with SMA of first N.
inline double ema_next(double prevEMA, double newX, const std::deque<double>& xs, size_t N) {
  if (N==0) return NaN();
  const double alpha = 2.0 / (double(N) + 1.0);
  if (!isFinite(prevEMA)) {
    // Initialize when we have N samples
    if (xs.size() < N) return NaN();
    return sma_lastN(xs, N);
  }
  return alpha * newX + (1.0 - alpha) * prevEMA;
}

// ------- VWAP (over last N ticks) -------
inline double vwap_lastN(const std::deque<double>& px, const std::deque<double>& vol, size_t N) {
  if (N==0 || px.size() < N || vol.size() < N) return NaN();
  double pv=0.0, v=0.0;
  for (size_t i=px.size()-N; i<px.size(); ++i) {
    pv += px[i] * vol[i];
    v  += vol[i];
  }
  if (v<=0.0) return NaN();
  return pv / v;
}

// ------- RSI (Wilder’s smoothing) -------
struct RsiState {
  double avgGain = NaN();
  double avgLoss = NaN();
  bool   init    = false;
  double lastPx  = NaN();
  size_t count   = 0;     // samples processed during init phase
};

inline double rsi_update(RsiState& st, double newPx, size_t period) {
  if (period==0) return NaN();
  if (!isFinite(st.lastPx)) { st.lastPx = newPx; return NaN(); }
  const double chg = newPx - st.lastPx;
  st.lastPx = newPx;

  double gain = chg > 0 ? chg : 0.0;
  double loss = chg < 0 ? -chg : 0.0;

  if (!st.init) {
    // Seed phase: accumulate running average over 'period' samples.
    if (!isFinite(st.avgGain)) st.avgGain = 0.0;
    if (!isFinite(st.avgLoss)) st.avgLoss = 0.0;
    st.avgGain = ((st.avgGain * (period - 1)) + gain) / double(period);
    st.avgLoss = ((st.avgLoss * (period - 1)) + loss) / double(period);
    ++st.count;
    if (st.count < period) return NaN();
    // Enough samples — transition to Wilder smoothing.
    // First RSI is computed from the seeded averages directly.
    st.init = true;
  } else {
    // Wilder smoothing for subsequent updates.
    st.avgGain = (st.avgGain * (period - 1) + gain) / double(period);
    st.avgLoss = (st.avgLoss * (period - 1) + loss) / double(period);
  }

  if (st.avgLoss <= 0.0) return 100.0;
  const double rs = st.avgGain / st.avgLoss;
  return 100.0 - (100.0 / (1.0 + rs));
}

} // namespace gma::ta
