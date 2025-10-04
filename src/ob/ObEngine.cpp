#include "gma/ob/ObEngine.hpp"
#include <cmath>
#include <limits>
#include <algorithm>

namespace gma::ob {
namespace {
inline double NaN(){ return std::numeric_limits<double>::quiet_NaN(); }
inline bool isFinite(double x){ return std::isfinite(x); }

const std::vector<Level>& side(const Snapshot& s, Side side){
  return (side==Side::Bid? s.bids.levels : s.asks.levels);
}

inline double quantize(double px, double tick){
  if(tick<=0) return px;
  double q = std::round(px / tick);
  return q * tick;
}

template<typename It, typename Getter, typename ReduceOp>
double reduce(It beg, It end, Getter g, ReduceOp op, double init, bool needFinite=true){
  double acc = init; bool any=false;
  for(auto it=beg; it!=end; ++it){
    double v = g(*it);
    if(needFinite && !isFinite(v)) continue;
    if(!any){ acc = v; any=true; } else acc = op(acc, v);
  }
  return any? acc : NaN();
}

double sum(It beg, It end, auto get) {
  double acc=0.0; bool any=false;
  for(auto it=beg; it!=end; ++it){ double v=get(*it); if(!isFinite(v)) continue; acc+=v; any=true; }
  return any? acc : NaN();
}

} // anon

double bestPrice(const Snapshot& s, Side sd){
  const auto& v = side(s, sd);
  if(v.empty()) return NaN();
  return v.front().price;
}
double bestSize(const Snapshot& s, Side sd){
  const auto& v = side(s, sd);
  if(v.empty()) return NaN();
  return v.front().size;
}
double spread(const Snapshot& s){
  if(s.bids.levels.empty() || s.asks.levels.empty()) return NaN();
  return s.asks.levels.front().price - s.bids.levels.front().price;
}
double mid(const Snapshot& s){
  if(s.bids.levels.empty() || s.asks.levels.empty()) return NaN();
  return 0.5*(s.asks.levels.front().price + s.bids.levels.front().price);
}

double levelIdx(const Snapshot& s, const LevelIdx& k){
  const auto& v = side(s, k.side);
  if(k.n<1 || (size_t)k.n>v.size()) return NaN();
  const Level& L = v[size_t(k.n)-1];
  switch(k.attr){
    case Target::Price: return L.price;
    case Target::Size:  return L.size;
    case Target::Orders: return L.orders;
    case Target::Notional: return isFinite(L.notional) ? L.notional : (L.price * L.size);
    default: return NaN();
  }
}

double levelPx(const Snapshot& s, const LevelPx& k, double tick){
  const auto& v = side(s, k.side);
  const double p = quantize(k.px, tick);
  double size = 0.0, orders = 0.0; bool any=false, anyOrders=false;
  for(const auto& L: v){
    if(std::abs(L.price - p) < 1e-12){
      any=true; size += L.size;
      if(isFinite(L.orders)){ anyOrders=true; orders += L.orders; }
    }
  }
  if(!any) return NaN();
  switch(k.attr){
    case Target::Price: return p;
    case Target::Size:  return size;
    case Target::Orders: return anyOrders? orders : NaN();
    case Target::Notional: return p * size;
    default: return NaN();
  }
}

static double reduceVector(const std::vector<Level>& vec, size_t a, size_t b, Reduce red, Target tgt){
  if(vec.empty() || a<1 || b<a) return NaN();
  a = std::max<size_t>(1, a); b = std::min<size_t>(b, vec.size());
  auto beg = vec.begin() + (a-1);
  auto end = vec.begin() + b;
  auto get = [&](const Level& L)->double{
    switch(tgt){
      case Target::Price: return L.price;
      case Target::Size: return L.size;
      case Target::Orders: return L.orders;
      case Target::Notional: return std::isfinite(L.notional)? L.notional : (L.price * L.size);
      default: return NaN();
    }
  };
  if(red==Reduce::Count){
    return double(std::distance(beg, end));
  }
  if(red==Reduce::Sum){
    double acc=0.0; bool any=false;
    for(auto it=beg; it!=end; ++it){ double v=get(*it); if(!isFinite(v)) continue; acc+=v; any=true; }
    return any? acc : NaN();
  }
  if(red==Reduce::Avg){
    double s=0.0; size_t n=0;
    for(auto it=beg; it!=end; ++it){ double v=get(*it); if(!isFinite(v)) continue; s+=v; n++; }
    return n? (s/double(n)) : NaN();
  }
  if(red==Reduce::Min){
    double m=NaN(); bool any=false;
    for(auto it=beg; it!=end; ++it){ double v=get(*it); if(!isFinite(v)) continue; m= any? std::min(m,v):v; any=true; }
    return any? m : NaN();
  }
  if(red==Reduce::Max){
    double m=NaN(); bool any=false;
    for(auto it=beg; it!=end; ++it){ double v=get(*it); if(!isFinite(v)) continue; m= any? std::max(m,v):v; any=true; }
    return any? m : NaN();
  }
  return NaN();
}

double rangeIdxReduce(const Snapshot& s, const RangeIdxSpec& r){
  const auto& vec = side(s, r.side);
  return reduceVector(vec, size_t(r.lv.a), size_t(r.lv.b), r.reduce, r.target);
}

double rangePxReduce(const Snapshot& s, const RangePxSpec& r, double tick){
  const auto& vec = side(s, r.side);
  const double p1 = quantize(r.p1, tick), p2 = quantize(r.p2, tick);
  std::vector<Level> sel; sel.reserve(vec.size());
  for(const auto& L: vec){
    if(L.price + 1e-12 < p1) continue;
    if(L.price - 1e-12 > p2) break;
    sel.push_back(L);
  }
  if(sel.empty()) return NaN();
  // Map price band to "levels 1..N" in sel and reuse reducer.
  RangeIdxSpec tmp{ r.side, {1, int(sel.size())}, r.reduce, r.reduce==Reduce::Count? Target::None : r.target };
  return reduceVector(sel, 1, sel.size(), r.reduce, tmp.target);
}

double cumLevels(const Snapshot& s, Side side, int N, Target tgt){
  if(N<1) return NaN();
  RangeIdxSpec r{ side, {1, N}, Reduce::Sum, tgt };
  return rangeIdxReduce(s, r);
}

double vwapLevels(const Snapshot& s, Side side, Range R){
  const auto& vec = side(s, side);
  if(vec.empty() || R.a<1) return NaN();
  size_t a = std::max(1, R.a), b = std::min<int>(R.b, int(vec.size()));
  if(b<a) return NaN();
  double pxsz=0.0, sz=0.0;
  for(size_t i=a-1;i<b;i++){ const auto& L=vec[i]; if(!isFinite(L.size)) continue; pxsz += L.price*L.size; sz += L.size; }
  return (sz>0.0)? (pxsz/sz) : NaN();
}

double vwapPriceBand(const Snapshot& s, Side sd, double p1, double p2, double tick){
  const auto& vec = side(s, sd);
  p1 = quantize(p1, tick); p2 = quantize(p2, tick);
  double pxsz=0.0, sz=0.0;
  for(const auto& L: vec){ if(L.price + 1e-12 < p1) continue; if(L.price - 1e-12 > p2) break; pxsz += L.price*L.size; sz += L.size; }
  return (sz>0.0)? (pxsz/sz) : NaN();
}

double imbalanceLevels(const Snapshot& s, Range R){
  double bid = cumLevels(s, Side::Bid, R.b, Target::Size) - cumLevels(s, Side::Bid, R.a-1, Target::Size);
  if(!isFinite(bid)){ bid = cumLevels(s, Side::Bid, R.b, Target::Size); } // fallback if a=1
  double ask = cumLevels(s, Side::Ask, R.b, Target::Size) - cumLevels(s, Side::Ask, R.a-1, Target::Size);
  if(!isFinite(ask)){ ask = cumLevels(s, Side::Ask, R.b, Target::Size); }
  if(!isFinite(bid) || !isFinite(ask)) return NaN();
  const double den = bid + ask; if(den<=0.0) return NaN();
  return (bid - ask) / den;
}

double imbalanceBand(const Snapshot& s, double p1, double p2, double tick){
  p1 = quantize(p1, tick); p2 = quantize(p2, tick);
  auto sumBand = [&](Side sd)->double{
    const auto& vec = side(s, sd);
    double sz=0.0; for(const auto& L: vec){ if(L.price + 1e-12 < p1) continue; if(L.price - 1e-12 > p2) break; sz += L.size; }
    return sz;
  };
  double bid = sumBand(Side::Bid);
  double ask = sumBand(Side::Ask);
  const double den = bid + ask; if(den<=0.0) return NaN();
  return (bid - ask) / den;
}

double meta(const Snapshot& s, const std::string& f){
  if(f=="seq") return double(s.meta.seq);
  if(f=="epoch") return double(s.meta.epoch);
  if(f=="is_stale") return s.meta.stale ? 1.0 : 0.0;
  if(f=="levels.bid") return double(s.meta.bidLevels);
  if(f=="levels.ask") return double(s.meta.askLevels);
  if(f=="last_change_ms") return double(s.meta.lastChangeMs);
  return NaN();
}

double eval(const Snapshot& s, const ObKey& k){
  switch(k.metric){
    case Metric::Spread: return spread(s);
    case Metric::Mid:    return mid(s);
    case Metric::Best:
      return (k.bestAttr==Target::Price) ? bestPrice(s, k.bestSide) : bestSize(s, k.bestSide);
    case Metric::LevelIdx: return levelIdx(s, k.levelIdx);
    case Metric::LevelPx:  return levelPx(s, k.levelPx, /*tick*/1.0); // tick handled in provider
    case Metric::RangeIdx: return rangeIdxReduce(s, k.rangeIdx);
    case Metric::RangePx:  return rangePxReduce(s, k.rangePx, /*tick*/1.0);
    case Metric::Cum:      return cumLevels(s, k.cumSide, k.cumN, k.cumTarget);
    case Metric::VWAP:
      return k.vwapByLevels ? vwapLevels(s, k.vwapSide, k.vwapLv)
                            : vwapPriceBand(s, k.vwapSide, k.vwapP1, k.vwapP2, /*tick*/1.0);
    case Metric::Imbalance:
      return k.imbByLevels ? imbalanceLevels(s, k.imbLv)
                           : imbalanceBand(s, k.imbP1, k.imbP2, /*tick*/1.0);
    case Metric::Meta:     return meta(s, k.metaField);
  }
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace gma::ob
