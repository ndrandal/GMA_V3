#include "gma/ob/ObMaterializer.hpp"

#include <algorithm>
#include <limits>
#include <cmath>

namespace gma::ob {

static inline const Ladder& bySide(const Snapshot& s, Side side) {
  return (side == Side::Bid) ? s.bids : s.asks;
}

// ------------------------------------------------------------
// Basic helpers
// ------------------------------------------------------------

double bestPrice(const Snapshot& s, Side side) {
  const auto& L = bySide(s, side);
  if (L.levels.empty())
    return std::numeric_limits<double>::quiet_NaN();
  return L.levels.front().price;
}

double bestSize(const Snapshot& s, Side side) {
  const auto& L = bySide(s, side);
  if (L.levels.empty())
    return 0.0;
  return L.levels.front().size;
}

double spread(const Snapshot& s) {
  if (s.bids.levels.empty() || s.asks.levels.empty())
    return std::numeric_limits<double>::quiet_NaN();
  return s.asks.levels.front().price - s.bids.levels.front().price;
}

double mid(const Snapshot& s) {
  if (s.bids.levels.empty() || s.asks.levels.empty())
    return std::numeric_limits<double>::quiet_NaN();
  return 0.5 * (s.asks.levels.front().price + s.bids.levels.front().price);
}

// ------------------------------------------------------------
// Level-based metrics
// ------------------------------------------------------------

double levelIdx(const Snapshot& s, const LevelIdx& spec) {
  const auto& L = bySide(s, spec.side);
  if (spec.n <= 0 || static_cast<size_t>(spec.n) > L.levels.size())
    return std::numeric_limits<double>::quiet_NaN();

  const auto& lvl = L.levels[spec.n - 1];

  switch (spec.attr) {
    case Target::Price:    return lvl.price;
    case Target::Size:     return lvl.size;
    case Target::Orders:   return lvl.orders;
    case Target::Notional: return lvl.notional;
    default:               return std::numeric_limits<double>::quiet_NaN();
  }
}

double levelPx(const Snapshot& s, const LevelPx& spec, double /*tick*/) {
  const auto& L = bySide(s, spec.side);
  for (const auto& lvl : L.levels) {
    if (lvl.price == spec.px) {
      switch (spec.attr) {
        case Target::Price:    return lvl.price;
        case Target::Size:     return lvl.size;
        case Target::Orders:   return lvl.orders;
        case Target::Notional: return lvl.notional;
        default:               return std::numeric_limits<double>::quiet_NaN();
      }
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// ------------------------------------------------------------
// Range metrics (by level index)
// ------------------------------------------------------------

double rangeIdxReduce(const Snapshot& s, const RangeIdxSpec& spec) {
  const auto& L = bySide(s, spec.side);
  if (L.levels.empty())
    return std::numeric_limits<double>::quiet_NaN();

  const int lo = std::max(1, spec.lv.a);
  const int hi = std::min(static_cast<int>(L.levels.size()), spec.lv.b);
  if (lo > hi)
    return std::numeric_limits<double>::quiet_NaN();

  double acc = 0.0;
  int count = 0;

  for (int i = lo - 1; i < hi; ++i) {
    const auto& lvl = L.levels[i];
    double v = 0.0;

    switch (spec.target) {
      case Target::Price:    v = lvl.price; break;
      case Target::Size:     v = lvl.size; break;
      case Target::Orders:   v = lvl.orders; break;
      case Target::Notional: v = lvl.notional; break;
      default: continue;
    }

    acc += v;
    ++count;
  }

  if (spec.reduce == Reduce::Avg && count > 0)
    return acc / count;

  return acc;
}

// ------------------------------------------------------------
// Range metrics (by price band)
// ------------------------------------------------------------

double rangePxReduce(const Snapshot& s, const RangePxSpec& spec, double /*tick*/) {
  const auto& L = bySide(s, spec.side);

  double acc = 0.0;
  double qty = 0.0;
  int count = 0;

  for (const auto& lvl : L.levels) {
    if (lvl.price < spec.p1 || lvl.price > spec.p2)
      continue;

    double v = 0.0;
    switch (spec.target) {
      case Target::Price:    v = lvl.price; break;
      case Target::Size:     v = lvl.size; break;
      case Target::Orders:   v = lvl.orders; break;
      case Target::Notional: v = lvl.notional; break;
      default: continue;
    }

    if (spec.reduce == Reduce::Avg) {
      acc += v;
      ++count;
    } else if (spec.reduce == Reduce::Sum) {
      acc += v;
    } else if (spec.reduce == Reduce::Count) {
      ++count;
    }
  }

  if (spec.reduce == Reduce::Avg && count > 0)
    return acc / count;

  if (spec.reduce == Reduce::Count)
    return static_cast<double>(count);

  return acc;
}

// ------------------------------------------------------------
// Imbalance
// ------------------------------------------------------------

double imbalanceLevels(const Snapshot& s, Range r) {
  double bidQty = 0.0;
  double askQty = 0.0;

  const int lo = std::max(1, r.a);
  const int hi = r.b;

  for (int i = lo - 1; i < hi; ++i) {
    if (i < static_cast<int>(s.bids.levels.size()))
      bidQty += s.bids.levels[i].size;
    if (i < static_cast<int>(s.asks.levels.size()))
      askQty += s.asks.levels[i].size;
  }

  const double sum = bidQty + askQty;
  if (sum <= 0.0)
    return 0.0;

  return (bidQty - askQty) / sum;
}

double imbalanceBand(const Snapshot& s, double p1, double p2, double /*tick*/) {
  double bidQty = 0.0;
  double askQty = 0.0;

  for (const auto& l : s.bids.levels)
    if (l.price >= p1 && l.price <= p2)
      bidQty += l.size;

  for (const auto& l : s.asks.levels)
    if (l.price >= p1 && l.price <= p2)
      askQty += l.size;

  const double sum = bidQty + askQty;
  if (sum <= 0.0)
    return 0.0;

  return (bidQty - askQty) / sum;
}

// ------------------------------------------------------------
// Dispatcher
// ------------------------------------------------------------

double eval(const Snapshot& s, const ObKey& k) {
  switch (k.metric) {
    case Metric::Best:
      return (k.bestSide == Side::Bid)
             ? bestPrice(s, Side::Bid)
             : bestPrice(s, Side::Ask);

    case Metric::LevelIdx:
      return levelIdx(s, k.levelIdx);

    case Metric::LevelPx:
      return levelPx(s, k.levelPx, 0.0);

    case Metric::RangeIdx:
      return rangeIdxReduce(s, k.rangeIdx);

    case Metric::RangePx:
      return rangePxReduce(s, k.rangePx, 0.0);

    case Metric::Imbalance:
      return k.imbByLevels
             ? imbalanceLevels(s, k.imbLv)
             : imbalanceBand(s, k.imbP1, k.imbP2, 0.0);

    case Metric::Spread:
      return spread(s);

    case Metric::Mid:
      return mid(s);

    case Metric::Meta:
      return meta(s, k.metaField);

    default:
      return std::numeric_limits<double>::quiet_NaN();
  }
}

} // namespace gma::ob
