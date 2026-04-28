#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

namespace gma {

/**
 * Dispatcher routes incoming events to subscribed listeners and maintains
 * per-field raw-value history so FunctionMap builtins (mean, sum, stddev, …)
 * can be recomputed on each update.
 *
 * Domain-specific computations (market TA, order-book derivations, …) live in
 * IEventComputer implementations owned by the dispatcher. They are invoked on
 * every onTick BEFORE the raw-field notify loop runs. Computers may call
 * notifyListeners() to deliver their computed values to subscribers.
 *
 * Renamed to engine::Dispatcher in a later step — this class will become the
 * generic routing layer. Until then, the market tick computer is wired in by
 * the constructor for backward compatibility with existing call sites.
 */
class Dispatcher {
public:
  Dispatcher(gma::rt::ThreadPool* threadPool, AtomicStore* store,
                   const util::Config& cfg = util::Config{});

  // Factory hook installed by the market side at boot. Every new dispatcher
  // calls this (if installed) to populate its computers list — lets the engine
  // header avoid any market include while preserving existing construction
  // call sites. Removed in Step 8 when IConnector formalization lands.
  using DefaultComputerFactory =
      std::function<std::vector<std::unique_ptr<engine::IEventComputer>>(
          const util::Config&)>;
  static void setDefaultComputerFactory(DefaultComputerFactory f);

  // Append an event computer after construction. Primarily used by tests and
  // code paths that want TA without depending on the default-factory hook.
  void addComputer(std::unique_ptr<engine::IEventComputer> computer);

  void registerListener(const std::string& symbol,
                        const std::string& field,
                        std::shared_ptr<INode> listener);

  void unregisterListener(const std::string& symbol,
                          const std::string& field,
                          std::shared_ptr<INode> listener);

  // Generic event ingress. Invokes every registered computer, then fans the
  // raw payload fields out to direct-field subscribers.
  void onTick(const Event& tick);

  // Public hook that IEventComputer implementations call to deliver a computed
  // value to listeners subscribed on (symbol, field). Snapshot semantics — the
  // listener lock is held only while copying subscriber shared_ptrs.
  void notifyListeners(const std::string& symbol,
                       const std::string& field,
                       double value);

private:
  void computeAndStoreAtomics(const std::string& symbol,
                              const std::string& field,
                              const std::vector<double>& history);

private:
  // Per-field history buffers per (symbol, field)
  std::unordered_map<
      std::string,
      std::unordered_map<std::string, std::deque<double>>
  > _histories;

  // Listener lists per (symbol, field)
  std::unordered_map<
      std::string,
      std::map<std::string, std::vector<std::shared_ptr<INode>>>
  > _listeners;

  // Domain-specific computers (e.g. MarketTickComputer for TA). Constructed in
  // the ctor; invoked on every onTick in registration order.
  std::vector<std::unique_ptr<engine::IEventComputer>> _computers;

  mutable std::shared_mutex _histMutex;
  mutable std::shared_mutex _listenerMutex;
  gma::rt::ThreadPool* _threadPool;
  AtomicStore*         _store;
  util::Config         _cfg;
  std::size_t          _maxHistory;
  std::size_t          _maxSymbols;
  std::size_t          _maxFieldsPerSymbol;
};

} // namespace gma
