#include "gma/ob/ObProvider.hpp"
#include "gma/ob/ObEngine.hpp"
#include <limits>

namespace gma::ob {
namespace {
inline double NaN(){ return std::numeric_limits<double>::quiet_NaN(); }

inline size_t needLevels(const ObKey& k){
  switch(k.metric){
    case Metric::Best: return 1;
    case Metric::Spread: return 1;
    case Metric::Mid: return 1;
    case Metric::LevelIdx: return size_t(k.levelIdx.n);
    case Metric::Cum: return size_t(k.cumN);
    case Metric::RangeIdx: return size_t(k.rangeIdx.lv.b);
    case Metric::VWAP: if(k.vwapByLevels) return size_t(k.vwapLv.b); else return 0;
    case Metric::Imbalance: if(k.imbByLevels) return size_t(k.imbLv.b); else return 0;
    case Metric::LevelPx: return 0;
    case Metric::RangePx: return 0;
    case Metric::Meta: return 1;
  }
  return 0;
}
} // anon

double Provider::get(const std::string& symbol, const std::string& keyStr) const {
  auto parsed = parseObKey(keyStr);
  if(!parsed) return NaN();
  auto key = *parsed;

  const size_t defLevels = (key.mode==Mode::Per ? defPer_ : defAgg_);
  size_t n = needLevels(key);
  std::optional<std::pair<double,double>> band;

  // Prices/bands need tick quantization in engine; pass real tick via meta field piggyback:
  const double tick = src_->tickSize(symbol);

  // Use price band when possible to minimize capture.
  if(key.metric==Metric::LevelPx){
    double p = key.levelPx.px;
    band = std::make_pair(p, p);
  } else if(key.metric==Metric::RangePx){
    band = std::make_pair(key.rangePx.p1, key.rangePx.p2);
  } else if(key.metric==Metric::VWAP && !key.vwapByLevels){
    band = std::make_pair(key.vwapP1, key.vwapP2);
  } else if(key.metric==Metric::Imbalance && !key.imbByLevels){
    band = std::make_pair(key.imbP1, key.imbP2);
  }

  auto snap = src_->capture(symbol, n? n : defLevels, key.mode, band);

  // Inject actual tick into evaluation for price-based functions by temporarily scaling:
  // We reuse the same eval but swap tick-sensitive helpers via wrapper calls.
  switch(key.metric){
    case Metric::LevelPx:   return levelPx(snap, key.levelPx, tick);
    case Metric::RangePx:   return rangePxReduce(snap, key.rangePx, tick);
    case Metric::VWAP:
      if(!key.vwapByLevels) return vwapPriceBand(snap, key.vwapSide, key.vwapP1, key.vwapP2, tick);
      break;
    case Metric::Imbalance:
      if(!key.imbByLevels)  return imbalanceBand(snap, key.imbP1, key.imbP2, tick);
      break;
    default: break;
  }
  return eval(snap, key);
}

} // namespace gma::ob
