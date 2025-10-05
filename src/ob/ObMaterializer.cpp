#include "gma/ob/ObMaterializer.hpp"
#include <algorithm>
#include <limits>

namespace gma::ob {

const Ladder& Materializer::bySide(const ObSnapshot& s, Side sd) {
  return sd == Side::Bid ? s.bids : s.asks;
}

double Materializer::clampLevelPx(const Ladder& L, size_t idx) {
  if (L.empty()) return std::numeric_limits<double>::quiet_NaN();
  if (idx >= L.size()) idx = L.size() - 1;
  return L[idx].px;
}

std::pair<size_t,size_t> Materializer::clampRange(const Ladder& L, size_t lo, size_t hi) {
  const size_t n = L.size();
  lo = std::min(lo, n);
  hi = std::min(hi, n);
  if (lo > hi) std::swap(lo, hi);
  return {lo, hi};
}

// ---------------- Metric surface ----------------

double Materializer::levelPx(const ObSnapshot& snap, LevelPxSpec spec, double /*tick*/) {
  const auto& L = bySide(snap, spec.side);
  return clampLevelPx(L, spec.index);
}

double Materializer::rangePxReduce(const ObSnapshot& snap, RangePxSpec spec, double /*tick*/) {
  const auto& L = bySide(snap, spec.side);
  auto [lo, hi] = clampRange(L, spec.lo, spec.hi);
  if (lo >= hi) return 0.0;

  // Example reduction: VWAP of the [lo, hi) band.
  double qsum = 0.0, pxq = 0.0;
  for (size_t i = lo; i < hi; ++i) {
    const auto& r = L[i];
    qsum += r.qty;
    pxq  += r.px * r.qty;
  }
  return qsum > 0.0 ? pxq / qsum : 0.0;
}

double Materializer::imbalanceLevels(const ObSnapshot& snap, RangeSpec spec) {
  const auto& bid = snap.bids;
  const auto& ask = snap.asks;

  const auto& L = (spec.side == Side::Bid) ? bid : ask;
  const size_t n = std::min(spec.levels, L.size());
  if (n == 0) return 0.0;

  double bidQty = 0.0, askQty = 0.0;
  for (size_t i = 0; i < n; ++i) {
    if (i < bid.size()) bidQty += bid[i].qty;
    if (i < ask.size()) askQty += ask[i].qty;
  }

  const double sum = bidQty + askQty;
  if (sum <= 0.0) return 0.0;
  // Standard imbalance: (bid - ask) / (bid + ask), but could be side-weighted.
  return (bidQty - askQty) / sum;
}

double Materializer::spreadPx(const ObSnapshot& snap, double /*tick*/) {
  if (snap.asks.empty() || snap.bids.empty())
    return std::numeric_limits<double>::quiet_NaN();
  return snap.asks.front().px - snap.bids.front().px;
}

double Materializer::midPx(const ObSnapshot& snap, double /*tick*/) {
  if (snap.asks.empty() || snap.bids.empty())
    return std::numeric_limits<double>::quiet_NaN();
  return 0.5 * (snap.asks.front().px + snap.bids.front().px);
}

// ---------------- Legacy-overload surface ----------------

double Materializer::levelPx(const ObSnapshot& snap, size_t levelIdx, double tick) {
  // Legacy assumed ask-side. Prefer the spec’ed overload in new code.
  return levelPx(snap, LevelPxSpec{Side::Ask, levelIdx}, tick);
}

double Materializer::rangePxReduce(const ObSnapshot& snap, std::pair<size_t,size_t> band, double tick) {
  // Legacy assumed ask-side. Prefer the spec’ed overload in new code.
  return rangePxReduce(snap, RangePxSpec{Side::Ask, band.first, band.second}, tick);
}

double Materializer::imbalanceLevels(const ObSnapshot& snap, size_t levels) {
  // Legacy: compute (“top N levels”) imbalance using both sides; side field is irrelevant here
  return imbalanceLevels(snap, RangeSpec{Side::Bid, levels});
}

// ---------------- Dispatcher ----------------

double Materializer::evaluate(const ObSnapshot& snap, const ObKey& ok, double tick) {
  switch (ok.metric) {
    case Metric::LevelPx:
      return levelPx(snap, ok.levelPx, tick);

    case Metric::RangePx:
      return rangePxReduce(snap, ok.rangePx, tick);

    case Metric::Imbalance:
      if (ok.imbByLevels) {
        return imbalanceLevels(snap, RangeSpec{ok.levelPx.side, ok.imbLv});
      } else {
        return rangePxReduce(snap, RangePxSpec{ok.levelPx.side, ok.imbP1, ok.imbP2}, tick);
      }

    case Metric::Spread:
      return spreadPx(snap, tick);

    case Metric::Mid:
      return midPx(snap, tick);
  }
  // Should be unreachable:
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace gma::ob
