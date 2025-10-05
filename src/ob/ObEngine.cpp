#include "gma/ob/ObMaterializer.hpp"
#include <algorithm>
#include <limits>

namespace gma::ob {

// Handy accessors so you can call side(s, Side::Bid/Ask)
static inline const Ladder& side(const ObSnapshot& s, Side which) {
  return which == Side::Bid ? s.bids : s.asks;
}

// --- Helpers that previously shadowed the function name with a parameter named `side` ---

static double vwapLevels(const ObSnapshot& snap, Side sd, RangeSpec R) {
  const auto& L = side(snap, sd);
  const size_t n = std::min(R.levels, L.size());
  if (n == 0) return 0.0;

  double qsum = 0.0, pxq = 0.0;
  for (size_t i = 0; i < n; ++i) {
    qsum += L[i].qty;
    pxq  += L[i].px * L[i].qty;
  }
  return qsum > 0.0 ? pxq / qsum : 0.0;
}

static double imbalanceLevels(const ObSnapshot& snap, Side /*sd*/, size_t Lcnt) {
  // Top-N across *both* sides (typical definition). “sd” is not used here.
  const size_t n = Lcnt;
  const auto Nbid = std::min(n, snap.bids.size());
  const auto Nask = std::min(n, snap.asks.size());

  double bidQty = 0.0, askQty = 0.0;
  for (size_t i = 0; i < Nbid; ++i) bidQty += snap.bids[i].qty;
  for (size_t i = 0; i < Nask; ++i) askQty += snap.asks[i].qty;

  const double sum = bidQty + askQty;
  if (sum <= 0.0) return 0.0;
  return (bidQty - askQty) / sum;
}

static double imbalanceBand(const ObSnapshot& snap, Side sd, RangePxSpec R) {
  // Just proxy to Materializer’s reduction (VWAP band or whatever you use),
  // but constrained to the given side.
  return Materializer::rangePxReduce(snap, R, /*tick*/ 0.0);
}

// If this file exposes an API, you can add a small façade that composes the above helpers.
// Otherwise, keep these as internal utilities used by your strategies.

} // namespace gma::ob
