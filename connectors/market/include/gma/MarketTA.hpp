// Market technical-analysis computations. Temporary home; moves into
// libgma_connector_market in Step 7 of the engine/connector refactor.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include <deque>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "gma/AtomicStore.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/market/MarketFieldMap.hpp"
#include "gma/util/Config.hpp"

namespace gma {

/**
 * Compute a suite of atomic values (statistics/indicators) for the given symbol
 * based on its price/volume history, storing results into AtomicStore.
 *
 * TA periods are read from cfg. Keys written depend on configured periods,
 * e.g. "sma_5", "sma_20" for cfg.taSMA={5,20}.
 *
 * Returns the computed (key, value) pairs so callers can notify listeners.
 */
std::vector<std::pair<std::string, ArgType>> computeAllAtomicValues(
    const std::string& symbol,
    const std::vector<TickEntry>& hist,
    AtomicStore& store,
    const util::Config& cfg = util::Config{}
);

// Per-symbol TA event computer. Owned by the market connector; one instance
// per dispatcher (state is not shared across dispatchers). The field-map
// argument tells the computer which JSON keys to read for trade price /
// volume / bid / ask / timestamp on each tick payload.
class MarketTickComputer final : public engine::IEventComputer {
public:
  // Default field-map (NASDAQ-style names) for callers that don't have a
  // configured one — primarily test code.
  explicit MarketTickComputer(const util::Config& cfg);

  // Explicit field-map. Used by MarketConnector when it threads the
  // connector-owned MarketFieldMap into the EventComputerRegistry factory.
  MarketTickComputer(const util::Config& cfg, market::MarketFieldMap fieldMap);

  std::string_view eventType() const override { return "tick"; }
  void compute(const Event& e, engine::ComputeContext& ctx) override;

private:
  util::Config                                       _cfg;
  market::MarketFieldMap                             _fieldMap;
  std::unordered_map<std::string, SymbolHistory>     _symbolHistories;
  std::unordered_set<std::string>                    _skipFields;
  mutable std::shared_mutex                          _histMutex;
  std::size_t                                        _maxHistory;
  std::size_t                                        _maxSymbols;
};

} // namespace gma
