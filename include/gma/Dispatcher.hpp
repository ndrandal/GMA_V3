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

#include <unordered_set>
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
 * Domain-specific computations live in IEventComputer implementations sourced
 * from EventComputerRegistry. The dispatcher caches per-type computer
 * instances lazily on first event of each type, so late-registered factories
 * are picked up automatically. Computers may call notifyListeners() to
 * deliver values to subscribers.
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
  //
  // CONCURRENCY CONTRACT (ENC-791/M11): onTick() may be called from multiple
  // ingress threads concurrently. The per-type computer cache is filled under a
  // lock, but IEventComputer::compute() runs OUTSIDE that lock, so the same
  // cached computer instance can have compute() entered concurrently for the
  // same event type. IEventComputer implementations (which live in connectors/)
  // MUST therefore make compute() safe to call concurrently — either stateless
  // or internally synchronised. The Dispatcher adds no further serialisation.
  void onTick(const Event& tick);

  // Public hook that IEventComputer implementations call to deliver a computed
  // value to listeners subscribed on (symbol, field). Snapshot semantics — the
  // listener lock is held only while copying subscriber shared_ptrs.
  void notifyListeners(const std::string& symbol,
                       const std::string& field,
                       double value);

private:
  // Recompute FunctionMap builtins over `history` and publish them.
  //
  // ATOMIC-KEY CONTRACT (ENC-792/M9, amended by ENC-1008).
  //
  // The stored key shape is a CLIENT-VISIBLE string, not an internal detail:
  // `field` in a WS subscribe request is read straight out of the request JSON
  // by TreeBuilder for both `Listener` and `AtomicAccessor` (TreeBuilder.cpp
  // ~257 and ~480), and AtomicAccessor resolves it against this store. There is
  // no version negotiation on it. That is why the shape is switchable by config
  // rather than simply changed.
  //
  // OFF — `cfg.atomicKeyNamespaceByField == false` (the default, and the
  // pre-ENC-1008 behaviour): builtin atomics live in a FLAT per-symbol namespace
  // keyed by bare function name (mean, sum, stddev, …) — the same key shape
  // Listeners bind to, AtomicAccessor reads, and MarketTA writes. `field` is not
  // folded into the key: a symbol is ASSUMED to have a single primary value
  // field driving its builtins. When that assumption does not hold — two fields
  // of one symbol both driving `mean` — they share the one key and the last
  // field written wins, silently losing the other. That is the ENC-1008 defect.
  //
  // ON — `cfg.atomicKeyNamespaceByField == true`: the stored key is
  // `<field>.<fn>` (`price.mean`, `size.mean`), so each source field keeps its
  // own derived values and neither is lost.
  //
  // What flipping it changes, exactly:
  //   * AtomicStore keys for DERIVED builtins move from `<fn>` to `<field>.<fn>`.
  //     An `AtomicAccessor` bound to a bare builtin name stops resolving — this
  //     is the breaking half, and the reason the default is OFF.
  //   * Listener PUSH is unchanged. Subscribers on the bare `<fn>` still receive
  //     every value exactly as before; `<field>.<fn>` becomes ADDITIONALLY
  //     subscribable. So the WS streaming surface is identical in both states.
  //   * Only builtins written by THIS function move. Connector-written atomics
  //     (MarketTA's bare `lastPrice`/`sma_5`/…, `ob.*`) and the ENC-1007 raw
  //     injected fields are untouched in both states.
  //   * More distinct store keys per symbol, so `maxFieldsPerSymbol` /
  //     AtomicStore::setCaps budgets are consumed faster.
  //   * The key is a plain `field + "." + fn` concatenation, so a field named
  //     after a provider namespace can collide with it (a field `ob` driving
  //     the builtin `spread` writes `ob.spread`, which ob::Provider owns).
  //     Narrow, opt-in-only, and documented in docs/atomic-keys.md.
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
  // Guarded by _computersMutex: addComputer() may append concurrently with
  // onTick() iterating (ENC-791/M8). Reads dominate, so a shared_mutex; onTick
  // snapshots the raw pointers under a shared lock and computes outside it.
  std::vector<std::unique_ptr<engine::IEventComputer>> _computers;
  mutable std::shared_mutex _computersMutex;

  // Per-type cache of computers built from EventComputerRegistry. Populated
  // lazily on first event of a given type — every `onTick` for an unseen
  // type calls EventComputerRegistry::createAll(type) and caches the result
  // for the lifetime of this Dispatcher. Late-registered factories are
  // therefore picked up on the first event of their type.
  std::unordered_map<std::string,
                     std::vector<std::unique_ptr<engine::IEventComputer>>>
                                          _computersByType;
  mutable std::mutex                      _computerCacheMx;

  // ENC-1007: admission ledger for RAW injected fields. `maxSymbols` is
  // documented as "maximum distinct symbols tracked before rejecting new
  // ones", so the raw-injection path has to honour it too — it must not be a
  // second, unbounded way into the AtomicStore. Kept separate from
  // `_histories` on purpose: admitting an injected field into the history map
  // would consume the per-symbol field budget that listened fields compete
  // for, changing which fields get histories. This ledger only records what
  // has been admitted, and holds no values.
  std::unordered_map<std::string, std::unordered_set<std::string>> _rawAdmitted;
  mutable std::shared_mutex _rawMutex;

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
