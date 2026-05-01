#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
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
 * IEventComputer implementations sourced from EventComputerRegistry. The
 * dispatcher caches per-type computer instances lazily on first event of
 * each type, so late-registered factories are picked up automatically.
 * Computers may call notifyListeners() to deliver values to subscribers.
 */
class Dispatcher {
public:
  Dispatcher(gma::rt::ThreadPool* threadPool, AtomicStore* store,
                   const util::Config& cfg = util::Config{});

  // Append an event computer after construction. Primarily used by tests and
  // code paths that want a computer without going through
  // EventComputerRegistry. Connectors should prefer the registry path so the
  // dispatcher's per-type cache picks them up automatically.
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

  // Computers added explicitly via addComputer(). Filtered by eventType() on
  // every onTick. Kept separate from the registry-driven cache so test code
  // can inject computers without touching the global EventComputerRegistry.
  std::vector<std::unique_ptr<engine::IEventComputer>> _computers;

  // Per-type cache of computers built from EventComputerRegistry. Populated
  // lazily on first event of a given type — every `onTick` for an unseen
  // type calls EventComputerRegistry::createAll(type) and caches the result
  // for the lifetime of this Dispatcher. Late-registered factories are
  // therefore picked up on the first event of their type.
  std::unordered_map<std::string,
                     std::vector<std::unique_ptr<engine::IEventComputer>>>
                                          _computersByType;
  mutable std::mutex                      _computerCacheMx;

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
