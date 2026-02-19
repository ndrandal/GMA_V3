#pragma once

#include <string>
#include <map>
#include <vector>
#include <deque>
#include <unordered_map>
#include <shared_mutex>
#include <memory>

#include "gma/SymbolTick.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/util/Config.hpp"

namespace gma {

/**
 * MarketDispatcher maintains:
 *  - A short history (deque) of raw numeric values per (symbol, field)
 *  - A SymbolHistory per symbol (price+volume for TA computations)
 *  - A registry of listeners per (symbol, field)
 * When a tick arrives, it:
 *  - extracts raw numeric fields (if any) from the JSON payload,
 *  - updates histories,
 *  - computes registered atomic functions over per-field history,
 *  - computes full TA suite (SMA/EMA/RSI/MACD/Bollinger/etc.) over symbol history,
 *  - writes all results to AtomicStore,
 *  - notifies listeners registered on either raw fields or atomic function names.
 */
class MarketDispatcher {
public:
  MarketDispatcher(gma::rt::ThreadPool* threadPool, AtomicStore* store,
                   const util::Config& cfg = util::Config{});

  // Subscribe/unsubscribe a node to a (symbol, field) key.
  void registerListener(const std::string& symbol,
                        const std::string& field,
                        std::shared_ptr<INode> listener);

  void unregisterListener(const std::string& symbol,
                          const std::string& field,
                          std::shared_ptr<INode> listener);

  // Ingest a JSON tick (payload is a RapidJSON Document with numeric fields).
  void onTick(const SymbolTick& tick);

private:
  // Recompute all atomic functions (from FunctionMap) for the (symbol, field) history.
  void computeAndStoreAtomics(const std::string& symbol,
                              const std::string& field,
                              const std::vector<double>& history);

  // Update symbol-level price/volume history and run full TA suite.
  void updateSymbolHistory(const std::string& symbol,
                           const SymbolTick& tick);

private:
  // Per-field history buffers per (symbol, field)
  std::unordered_map<
      std::string, // symbol
      std::unordered_map<
          std::string,       // field name
          std::deque<double> // history entries
      >
  > _histories;

  // Symbol-level price+volume history for full TA computation
  std::unordered_map<std::string, SymbolHistory> _symbolHistories;

  // Listener lists per (symbol, field)
  std::unordered_map<
      std::string, // symbol
      std::map<
          std::string,                         // field name (raw or function name)
          std::vector<std::shared_ptr<INode>>  // subscribers
      >
  > _listeners;

  mutable std::shared_mutex _histMutex;      // protects _histories and _symbolHistories
  mutable std::shared_mutex _listenerMutex;  // protects _listeners
  gma::rt::ThreadPool* _threadPool;          // for offloading work (not owned)
  AtomicStore* _store;                       // where atomic results are written (not owned)
  util::Config _cfg;                         // TA configuration
  static constexpr size_t MAX_HISTORY = 1000;
  static constexpr size_t MAX_SYMBOLS = 10000;
};

} // namespace gma
