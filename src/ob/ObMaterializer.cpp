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
  std::lock_guard<std::mutex> lk(mx_);
  cfg_ = cfg;
  running_ = true;
}
void Materializer::stop(){
  std::lock_guard<std::mutex> lk(mx_);
  running_ = false;
}

void Materializer::onOrderBookUpdate(const std::string& symbol, Mode mode){
  std::vector<std::string> keys;
  size_t maxL = (mode==Mode::Per ? cfg_.maxLevelsPer : cfg_.maxLevelsAgg);

  {
    std::lock_guard<std::mutex> lk(mx_);
    if(!running_) return;
    auto it = cfg_.keysBySymbol.find(symbol);
    keys = (it!=cfg_.keysBySymbol.end()) ? it->second : cfg_.defaultKeys;

    auto& st = perState_[symbol];
    auto now = std::chrono::steady_clock::now();
    if(cfg_.throttleMs>0){
      auto minNext = st.last + std::chrono::milliseconds(cfg_.throttleMs);
      if(now < minNext) return; // coalesce
    }
    st.last = now;
  }

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
      case Metric::VWAP:
        val = ok->vwapByLevels ? vwapLevels(snap, ok->vwapSide, ok->vwapLv)
                               : vwapPriceBand(snap, ok->vwapSide, ok->vwapP1, ok->vwapP2, tick);
        break;
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

} // namespace gma::ob
