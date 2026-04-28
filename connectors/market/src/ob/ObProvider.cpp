#include "gma/ob/ObProvider.hpp"
#include <vector>
#include <string>
#include <cmath>

namespace gma::ob {

static inline std::vector<std::string> split(const std::string& s, char d='.') {
  std::vector<std::string> out; std::string cur;
  for (char c: s) { if (c==d){ out.push_back(cur); cur.clear(); } else cur.push_back(c); }
  out.push_back(cur); return out;
}
static inline int toInt(const std::string& s){ return std::stoi(s); }

Provider::Provider(std::shared_ptr<FunctionalSnapshotSource> src,
                   std::size_t defPerLevels,
                   std::size_t defAggLevels)
: src_(std::move(src)), defPer_(defPerLevels), defAgg_(defAggLevels) {}

double Provider::get(const std::string& symbol, const std::string& fullKey) const {
  if (!src_) return std::numeric_limits<double>::quiet_NaN();

  // Guard against malformed keys — stoi() throws on non-numeric tokens,
  // and capture() could throw on degenerate inputs.
  try {
    return getImpl(symbol, fullKey);
  } catch (...) {
    return std::numeric_limits<double>::quiet_NaN();
  }
}

double Provider::getImpl(const std::string& symbol, const std::string& fullKey) const {

  // Fast path
  if (fullKey == "ob.spread") {
    auto snap = src_->capture(symbol, 1, Mode::Per, std::nullopt);
    if (snap.bids.levels.empty() || snap.asks.levels.empty())
      return std::numeric_limits<double>::quiet_NaN();
    return snap.asks.levels[0].price - snap.bids.levels[0].price;
  }

  auto parts = split(fullKey);

  // ob.level.bid.N.metric
  if (parts.size()==5 && parts[0]=="ob" && parts[1]=="level" && (parts[2]=="bid"||parts[2]=="ask")) {
    int N = toInt(parts[3]); if (N<=0) return std::numeric_limits<double>::quiet_NaN();
    const std::string& metric = parts[4];
    auto snap = src_->capture(symbol, std::max<std::size_t>(defPer_, (std::size_t)N), Mode::Per, std::nullopt);
    const auto& vec = (parts[2]=="bid") ? snap.bids.levels : snap.asks.levels;
    if ((int)vec.size() < N) return std::numeric_limits<double>::quiet_NaN();
    const auto& L = vec[N-1];
    if (metric=="price")    return L.price;
    if (metric=="size")     return L.size;
    if (metric=="notional") return L.notional;
    if (metric=="orders")   return L.orders;
    return std::numeric_limits<double>::quiet_NaN();
  }

  // ob.cum.bid.levels.K.metric
  if (parts.size()==6 && parts[0]=="ob" && parts[1]=="cum" && (parts[2]=="bid"||parts[2]=="ask") && parts[3]=="levels") {
    int K = toInt(parts[4]); if (K<=0) return std::numeric_limits<double>::quiet_NaN();
    const std::string& metric = parts[5];
    auto snap = src_->capture(symbol, std::max<std::size_t>(defPer_, (std::size_t)K), Mode::Per, std::nullopt);
    const auto& vec = (parts[2]=="bid") ? snap.bids.levels : snap.asks.levels;
    double acc=0.0; int n = std::min<int>(K, (int)vec.size());
    for (int i=0;i<n;++i) {
      if      (metric=="size")     acc += vec[i].size;
      else if (metric=="notional") acc += vec[i].notional;
      else if (metric=="orders")   acc += vec[i].orders;
      else return std::numeric_limits<double>::quiet_NaN();
    }
    return acc;
  }

  // ob.imbalance.levels.M-N
  if (parts.size()==4 && parts[0]=="ob" && parts[1]=="imbalance" && parts[2]=="levels") {
    auto range = parts[3];
    auto dash = range.find('-'); if (dash==std::string::npos) return std::numeric_limits<double>::quiet_NaN();
    int M = toInt(range.substr(0,dash)), N = toInt(range.substr(dash+1));
    if (M<=0 || N<M) return std::numeric_limits<double>::quiet_NaN();
    auto snap = src_->capture(symbol, std::max<std::size_t>(defPer_, (std::size_t)N), Mode::Per, std::nullopt);
    double b=0, a=0;
    int hi = std::min<int>(N, (int)std::min(snap.bids.levels.size(), snap.asks.levels.size()));
    for (int i=M-1;i<hi;++i) { b += snap.bids.levels[i].size; a += snap.asks.levels[i].size; }
    double den = b+a; if (den<=0) return 0.0;
    return (b-a)/den;
  }

  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace gma::ob
