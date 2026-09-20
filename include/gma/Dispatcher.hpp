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

  // ENC-1338 — DIAGNOSTIC SNAPSHOT of the nodes currently registered on
  // (symbol, field). Empty when nothing is registered there.
  //
  // Snapshot semantics, exactly like `notifyListeners`: the listener lock is
  // held only while copying the `shared_ptr`s out, and the returned vector is
  // a copy. It is NOT a synchronisation point and a registration may land or
  // be torn down the instant it returns — do not build routing on it.
  //
  // It exists so the strand a DAG is really running on can be observed from
  // OUTSIDE the DAG. `ClientSession::handleSubscribe` mints the request's
  // strand and hands it to `buildForRequest`, which hands it to every
  // `Listener` it builds and registers here; reading it back off a registered
  // `Listener` is the only route from a live WebSocket subscription to the
  // executor it was actually given. Used by
  // `tests/ws/SubscriptionStrandMintTest.cpp`.
  std::vector<std::shared_ptr<INode>> listenersFor(const std::string& symbol,
                                                   const std::string& field) const;

  // ENC-1291. How many subscriptions are live RIGHT NOW, across every
  // (symbol, field).
  //
  // This exists because SPEC specs/2026-09-20-gma-join-correctness section 1.5
  // — a rejected build stranding every `Listener` it had already registered,
  // an unbounded client-reachable leak — had no direct observation point. The
  // only way to see it was to tick and watch for a value arriving at a
  // terminal, which is blind whenever the stranded Listener's downstream is
  // already dead (its downstream is a weak_ptr, so a Listener whose downstream
  // died is still SUBSCRIBED and still costs `onTick` work, while delivering
  // nothing anyone can observe). That is exactly the shape a rejected fan-in
  // build leaves behind, and it is why `ArityMismatchIsRefusedBeforeAnything
  // Subscribes` was green against a builder that validated far too late.
  //
  // Read it as an observability accessor, not as engine state: nothing in
  // src/ calls it.
  std::size_t subscriptionCount() const;

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
  //
  // ORDERED DELIVERY REQUIRES SINGLE-THREADED INGRESS PER SYMBOL (ENC-1005,
  // SPEC specs/2026-09-20-gma-join-correctness D3). Read this as a PRECONDITION
  // of D3, not as a property of it.
  //
  // The per-request strand serialises everything DOWNSTREAM of a Listener, and
  // this method's fan-out is upstream of one. Within ONE onTick call the order
  // is deterministic — `_listeners`' inner container is a `std::map<field,...>`,
  // so `ask` precedes `bid` — and `deliver()` preserves it by calling a
  // strand-bearing Listener inline. Two CONCURRENT onTick calls interleave
  // their inline posts, and the strand then faithfully preserves an order that
  // was already scrambled here. Measured on a corpus-86 DAG: 0 of 2000 tuples
  // wrong with one ingress thread, 1596 of 2000 wrong with two (1444 of them
  // `ask` paired with `ask` — SPEC section 1.1 defect 4, back in full).
  //
  // It is not reachable in the shipped server: `src/main.cpp` runs `ioc.run()`
  // on exactly one thread and every ingress source hangs off that one
  // io_context. It is one `for (...) threads.emplace_back([&]{ioc.run();})`
  // away, and that is a change nobody would expect to break join correctness —
  // which is why it is written here, on the contract that permits it, rather
  // than left to be rediscovered. Making D3 hold under concurrent ingress needs
  // a decision ENC-1005 did not have the remit to take: either serialise
  // ingress per symbol, which the paragraph above explicitly refuses, or carry
  // a sequence number on the value — the provenance-token design SPEC D2
  // rejected. Raised on ENC-1005 for the SPEC to rule on.
  void onTick(const Event& tick);

  // Public hook that IEventComputer implementations call to deliver a computed
  // value to listeners subscribed on (symbol, field). Snapshot semantics — the
  // listener lock is held only while copying subscriber shared_ptrs.
  void notifyListeners(const std::string& symbol,
                       const std::string& field,
                       double value);

private:
  // ENC-1005 / SPEC D3. The single routing decision for every value this
  // Dispatcher hands to a subscriber: pool post (the default, unchanged) or an
  // inline call for a node that re-posts onto its own serializing executor.
  // Defined in Dispatcher.cpp with the full rationale.
  void deliver(const std::shared_ptr<INode>& node, const StreamValue& out);

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
  //     This is the breaking half, and the reason the default is OFF. It has
  //     two shapes, and the second is nastier:
  //       - For the 53 builtin names nothing else writes, an `AtomicAccessor`
  //         bound to the bare name stops resolving — a loud failure.
  //       - For `mean`, `median` and `spread`, which MarketTickComputer ALSO
  //         writes (over price history, MarketTA.cpp ~91-92 and ~391), the bare
  //         key keeps resolving and silently returns MarketTA's value instead
  //         of the Dispatcher's. Today the Dispatcher wins the key by running
  //         after the computers; with the flag on it vacates it. That also
  //         splits push from pull for those three names — a Listener on bare
  //         `mean` still gets the Dispatcher's per-field mean while an
  //         AtomicAccessor on bare `mean` now reads MarketTA's price mean.
  //         Pinned by AtomicKeyNamespaceTest
  //         .MarketTAKeepsTheBareKeyAliveWithADifferentValue.
  //   * Listener PUSH is unchanged. Subscribers on the bare `<fn>` still receive
  //     every value exactly as before; `<field>.<fn>` becomes ADDITIONALLY
  //     subscribable. So the WS streaming surface is identical in both states.
  //   * Only builtins written by THIS function move. Connector-written atomics
  //     (MarketTA's bare `lastPrice`/`sma_5`/…, `ob.*`) and the ENC-1007 raw
  //     injected fields are untouched in both states.
  //   * More distinct store keys per symbol, so `maxFieldsPerSymbol` /
  //     AtomicStore::setCaps budgets are consumed faster. The failure mode is a
  //     silent drop: past the cap AtomicStore::set discards NEW keys, so
  //     flipping this can stop some OTHER producer's key being admitted.
  //   * The key is a plain `field + "." + fn` concatenation, so a field named
  //     after an existing dotted key prefix can collide with it: a field `ob`
  //     driving the builtin `spread` writes `ob.spread` (ob::Provider's key),
  //     and a field `synthetic` driving `sin` writes `synthetic.sin`
  //     (SyntheticConnector's). Narrow, opt-in-only, documented in
  //     docs/atomic-keys.md.
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
