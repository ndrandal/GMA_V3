#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObProvider.hpp"
#include <limits>

namespace gma::ob {
namespace {
inline double NaN(){ return std::numeric_limits<double>::quiet_NaN(); }

int64_t now_ms(){
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
} // anon

void Materializer::start(const MaterializeConfig& cfg){
  {
    std::lock_guard<std::mutex> lk(mx_);
    cfg_ = cfg;
    running_ = true;
    stopping_.store(false, std::memory_order_release);
    wake_ = false;
  }

  if (cfg_.intervalMs <= 0) {
    // Immediate mode: no thread; caller triggers onOrderBookUpdate explicitly
    return;
  }

  // Periodic coalescing thread to allow batched materialization
  thr_ = std::thread([this](){
    while (!stopping_.load(std::memory_order_acquire)) {
      std::unique_lock<std::mutex> lk(mx_);
      cv_.wait_for(lk, std::chrono::milliseconds(cfg_.intervalMs), [this]{
        return wake_ || stopping_.load(std::memory_order_acquire);
      });
      wake_ = false;
      // nothing to do here directly; onOrderBookUpdate computes on demand per symbol
    }
  });
}

void Materializer::stop() {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return; // already stopping/stopped
  }
  {
    std::lock_guard<std::mutex> lk(mx_);
    wake_ = true;              // make sure the loop wakes if it's sleeping
  }
  cv_.notify_all();
  if (thr_.joinable()) {
    try { thr_.join(); } catch (...) {}
  }
  running_.store(false, std::memory_order_release);
}

void Materializer::onOrderBookUpdate(const std::string& symbol, Mode mode){
  std::vector<std::string> keys;
  size_t maxL = (mode==Mode::Per ? cfg_.maxLevelsPer : cfg_.maxLevelsAgg);

  {
    std::lock_guard<std::mutex> lk(mx_);
    if(!running_) return;
    auto it = cfg_.keysBySymbol.find(symbol);
    keys = (it!=cfg_.keysBySymbol.end()) ? it->second : cfg_.defaultKeys;
  }

  if(!src_ || !write_) return;
  if(keys.empty()) return;

  // Single snapshot covers all keys
  auto snap = src_->capture(symbol, maxL, mode, std::nullopt);
  const double tick = src_->tickSize(symbol);
  const int64_t ts  = now_ms();

  for(const auto& ks : keys){
    auto ok = parseObKey(ks);
    if(!ok) continue;
    double val = NaN();
    switch(ok->metric){
      case Metric::LevelPx:   val = levelPx(snap, ok->levelPx, tick); break;
      case Metric::RangePx:   val = rangePxReduce(snap, ok->rangePx, tick); break;
      case Metric::SpreadPx:  val = spreadPx(snap, tick); break;
      case Metric::MidPx:     val = midPx(snap, tick); break;
      case Metric::Imbalance:
        val = ok->imbByLevels ? imbalanceLevels(snap, ok->imbLv)
                              : imbalanceBand(snap, ok->imbP1, ok->imbP2, tick);
        break;
      default:
        val = eval(snap, *ok);
        break;
    }
    write_(symbol, ks, val, ts);
    if(notify_) (*notify_)(symbol, ks);
  }
}

// --- helpers ---------------------------------------------------------------

double Materializer::levelPx(const ObSnapshot& snap, size_t level, double tick){
  if (level==0) return NaN();
  double bid=NaN(), ask=NaN();
  if (snap.bids.size() >= level) bid = snap.bids[level-1].price * tick;
  if (snap.asks.size() >= level) ask = snap.asks[level-1].price * tick;
  return (!std::isnan(bid) ? bid : (!std::isnan(ask) ? ask : NaN()));
}

double Materializer::rangePxReduce(const ObSnapshot& snap, std::pair<size_t,size_t> lv, double tick){
  if (lv.first==0 || lv.second==0 || lv.first>snap.bids.size() || lv.second>snap.asks.size()) return NaN();
  double bestBid = snap.bids[lv.first-1].price * tick;
  double bestAsk = snap.asks[lv.second-1].price * tick;
  return bestAsk - bestBid;
}

double Materializer::spreadPx(const ObSnapshot& snap, double tick){
  if (snap.bids.empty() || snap.asks.empty()) return NaN();
  return (snap.asks.front().price - snap.bids.front().price) * tick;
}

double Materializer::midPx(const ObSnapshot& snap, double tick){
  if (snap.bids.empty() || snap.asks.empty()) return NaN();
  return 0.5 * (snap.asks.front().price + snap.bids.front().price) * tick;
}

double Materializer::imbalanceLevels(const ObSnapshot& snap, size_t uptoLevels){
  uptoLevels = std::min(uptoLevels, std::max(snap.bids.size(), snap.asks.size()));
  if (uptoLevels==0) return NaN();
  uint64_t bidSz=0, askSz=0;
  for (size_t i=0;i<uptoLevels;i++){
    if (i < snap.bids.size()) bidSz += snap.bids[i].size;
    if (i < snap.asks.size()) askSz += snap.asks[i].size;
  }
  if (bidSz==0 && askSz==0) return 0.0;
  return static_cast<double>(bidSz - askSz) / static_cast<double>(bidSz + askSz);
}

double Materializer::imbalanceBand(const ObSnapshot& snap, double pct1, double pct2, double tick){
  if (snap.bids.empty() || snap.asks.empty()) return NaN();
  const double mid = midPx(snap, tick);
  const double px1 = mid * (1.0 - pct1);
  const double px2 = mid * (1.0 + pct2);
  uint64_t bidSz=0, askSz=0;
  for (auto& b : snap.bids) {
    double p = b.price * tick;
    if (p >= px1) bidSz += b.size;
  }
  for (auto& a : snap.asks) {
    double p = a.price * tick;
    if (p <= px2) askSz += a.size;
  }
  if (bidSz==0 && askSz==0) return 0.0;
  return static_cast<double>(bidSz - askSz) / static_cast<double>(bidSz + askSz);
}

double Materializer::eval(const ObSnapshot& snap, const ParsedObKey& keySpec){
  switch(keySpec.metric){
    case Metric::SpreadPx: return spreadPx(snap, keySpec.tick);
    case Metric::MidPx:    return midPx(snap, keySpec.tick);
    default: break;
  }
  return NaN();
}

} // namespace gma::ob
