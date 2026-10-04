// include/gma/server/SharedSubscriptionRegistry.hpp
#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "gma/nodes/INode.hpp"
#include "gma/nodes/SharedTerminal.hpp"

namespace gma::server {

// ENC-1041. Cross-connection subscription dedup: one built request DAG per
// canonical request key, shared by every subscriber that resolves to that key,
// torn down when the last one leaves.
//
// Owned by `ExecutionContext`, which is the one object every `ClientSession` on
// a server already shares (it is also where the `AtomicStore` and the
// `ThreadPool` the DAG is built against live — so "same registry" and "same
// build inputs" are the same scope by construction, which is what makes
// sharing across connections sound in the first place).
//
// WHY NOT `gma::RequestRegistry` (include/gma/RequestRegistry.hpp). That class
// declares a plausible-looking `string -> shared_ptr<INode>` map under a
// `shared_mutex` and has no references anywhere in `src/` or `include/`, so the
// obvious move is to adopt it. It is the wrong shape in two ways that are not
// cosmetic, and both are PINNED BY ITS OWN TESTS
// (`tests/registry/RequestRegistryTest.cpp`, `tests/connection/ConnectionTest.cpp`):
//
//   * `registerRequest` on an existing id OVERWRITES and shuts the old root
//     down (`RequestRegistryTest.OverwriteRegister`). That is right for a
//     per-connection request id, where re-subscribing replaces the request. It
//     is exactly wrong for a shared key, where the second arrival must JOIN the
//     live DAG — overwriting would shut down a computation N-1 other
//     connections are still being fed from.
//   * `unregisterRequest` tears the entry down unconditionally
//     (`UnregisterRequestCallsShutdownOnlyForThat`). There is no refcount to
//     hold, so the first disconnect of N would kill the DAG for everybody.
//
// So this is a parallel mechanism, not a widening: `RequestRegistry`'s id ->
// owner semantics are left exactly as its tests describe them.
class SharedSubscriptionRegistry {
public:
  // What a builder produces: the DAG's head plus every node that must stay
  // alive for it (mirrors `tree::BuiltChain`, restated here so this header does
  // not have to pull in TreeBuilder.hpp and rapidjson behind it).
  struct Built {
    std::shared_ptr<INode>              head;
    std::vector<std::shared_ptr<INode>> keepAlive;
  };

  // Builds the DAG terminating at the supplied shared terminal. Called at most
  // once per live key, with NO registry lock held, so an expensive build never
  // blocks an unrelated subscribe. May throw; see `acquire`.
  using Builder = std::function<Built(std::shared_ptr<nodes::SharedTerminal>)>;

  // One subscriber's hold on a shared DAG. Move-only; releasing detaches that
  // subscriber's sink and tears the DAG down if it was the last.
  class Lease {
  public:
    Lease() = default;
    Lease(const Lease&)            = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& o) noexcept;
    Lease& operator=(Lease&& o) noexcept;
    ~Lease();

    void release() noexcept;
    bool valid() const noexcept { return reg_ != nullptr; }
    const std::string& key() const noexcept { return key_; }
    nodes::SharedTerminal::SubscriberId subscriberId() const noexcept { return id_; }

  private:
    friend class SharedSubscriptionRegistry;
    Lease(SharedSubscriptionRegistry* reg, std::string key,
          nodes::SharedTerminal::SubscriberId id)
      : reg_(reg), key_(std::move(key)), id_(id) {}

    SharedSubscriptionRegistry*         reg_{nullptr};
    std::string                         key_;
    nodes::SharedTerminal::SubscriberId id_{0};
  };

  SharedSubscriptionRegistry() = default;
  SharedSubscriptionRegistry(const SharedSubscriptionRegistry&)            = delete;
  SharedSubscriptionRegistry& operator=(const SharedSubscriptionRegistry&) = delete;
  ~SharedSubscriptionRegistry();

  // Attach `sink` to the DAG registered under `key`, building it with `build`
  // if there is none yet. Returns an invalid Lease only if `sink` is null.
  //
  // ATTRIBUTION (ENC-1396). `build` is run on the thread of the subscriber that
  // found the key missing, and anything it throws propagates out of `acquire`
  // UNCHANGED with no entry left behind. That is what keeps a build rejection
  // attributable to the request that caused it: `handleSubscribe`'s
  // `catch (const std::exception&) { sendError("build", ex.what(), key); }`
  // still sees its own exception with its own `RequestKey` in scope. A failed
  // build is never cached, so the next identical subscribe re-runs it and is
  // rejected under ITS key rather than inheriting somebody else's silence.
  //
  // CONCURRENCY. A key under construction is published to the map immediately
  // in a `Building` state; a second subscriber for the same key waits on a
  // condition variable rather than building a duplicate DAG. If the builder
  // throws, the entry is erased and the waiters are woken to retry (and will
  // typically fail the same way — under their own key, which is the point).
  // The registry lock is never held across a `build`, a `shutdown`, or a
  // downstream `onValue`.
  Lease acquire(const std::string& key, std::shared_ptr<INode> sink,
                const Builder& build);

  // ---- Introspection (tests / metrics) ----------------------------------

  // Keys with a live, fully-built DAG. Excludes keys still under construction.
  std::size_t liveKeys() const;
  // Subscribers attached to `key`, or 0 if there is no live DAG for it.
  std::size_t refCount(const std::string& key) const;
  // How many DAGs this registry has actually BUILT since construction. This is
  // the number the dedup test asserts: two identical subscribes must move it by
  // one, which no count of live keys or of subscribers can distinguish from
  // "built twice and one replaced the other".
  std::size_t buildCount() const;
  // How many `acquire` calls joined an EXISTING DAG instead of building one.
  std::size_t shareCount() const;

  // Shut every live DAG down and drop every entry. Leases outstanding at that
  // point become no-ops on release (their key is gone). Idempotent.
  void shutdownAll() noexcept;

private:
  struct Entry {
    // `Building` is visible in the map so a concurrent acquire waits instead of
    // building a second DAG for the same key.
    bool                                  building{true};
    std::shared_ptr<nodes::SharedTerminal> terminal;
    std::shared_ptr<INode>                 head;
    std::vector<std::shared_ptr<INode>>    keepAlive;
  };

  void release(const std::string& key,
               nodes::SharedTerminal::SubscriberId id) noexcept;

  mutable std::mutex      mu_;
  std::condition_variable cv_;
  // shared_ptr<Entry> rather than Entry: `acquire` keeps a reference to an entry
  // while it drops the lock to run the builder, and an unordered_map rehash
  // would otherwise invalidate it.
  std::unordered_map<std::string, std::shared_ptr<Entry>> entries_;
  std::size_t builds_{0};
  std::size_t shares_{0};
};

} // namespace gma::server
