#include "gma/ob/ObMaterializer.hpp"
#include <algorithm>

namespace gma::ob {

// Access ladder by side
static inline const Ladder& ladder(const Snapshot& s, Side which) {
  return (which == Side::Bid) ? s.bids : s.asks;
}

// ---------------- VWAP (levels) ----------------

static double vwapLevels(const Snapshot& snap, Side sd, Range R) {
  const auto& L = ladder(snap, sd).levels;

  const size_t n = std::min<size_t>(R.b, L.size());
  if (n == 0) return 0.0;

  double pxq = 0.0, qsum = 0.0;
  for (size_t i = 0; i < n; ++i) {
    pxq  += L[i].price * L[i].size;
    qsum += L[i].size;
  }
  return qsum > 0.0 ? pxq / qsum : 0.0;
}

// ---------------- Imbalance (levels) ----------------

static double imbalanceLevels(const Snapshot& snap, size_t levels) {
  const size_t nBid = std::min(levels, snap.bids.levels.size());
  const size_t nAsk = std::min(levels, snap.asks.levels.size());

  double bidQty = 0.0, askQty = 0.0;

  for (size_t i = 0; i < nBid; ++i)
    bidQty += snap.bids.levels[i].size;

  for (size_t i = 0; i < nAsk; ++i)
    askQty += snap.asks.levels[i].size;

  const double sum = bidQty + askQty;
  if (sum <= 0.0) return 0.0;

  return (bidQty - askQty) / sum;
}

// ---------------- Imbalance (price band) ----------------

static double imbalanceBand(const Snapshot& snap, Side sd, const RangePxSpec& R) {
  return rangePxReduce(snap, R, /*tick*/ 0.0);
}

} // namespace gma::ob
