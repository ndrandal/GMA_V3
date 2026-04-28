#pragma once
#include <vector>
#include <string>

namespace gma::ob {

inline std::vector<std::string> defaultProfile(){
  return {
    "ob.best.bid.price", "ob.best.bid.size",
    "ob.best.ask.price", "ob.best.ask.size",
    "ob.spread", "ob.mid",
    "ob.cum.bid.levels.10.size",
    "ob.cum.ask.levels.10.size",
    "ob.imbalance.levels.1-10",
    "ob.vwap.bid.levels.1-10",
    "ob.vwap.ask.levels.1-10",
    "ob.meta.seq", "ob.meta.is_stale"
  };
}

} // namespace gma::ob
