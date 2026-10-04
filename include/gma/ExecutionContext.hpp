// include/gma/ExecutionContext.hpp
#pragma once

#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"   // <-- IMPORTANT: rt::ThreadPool
#include "gma/server/SharedSubscriptionRegistry.hpp"

namespace gma {

class ExecutionContext {
public:
  ExecutionContext(AtomicStore* store, rt::ThreadPool* pool)
    : _store(store), _pool(pool) {}

  AtomicStore* store() const noexcept { return _store; }
  rt::ThreadPool* pool() const noexcept { return _pool; }

  // ENC-1041. Cross-connection subscription dedup lives here, not on
  // `WebSocketServer`, for two reasons that are the same reason:
  //
  //  * This is the object that already defines the SCOPE within which two
  //    subscriptions have identical build inputs. A shared DAG is built once
  //    against one `AtomicStore` and one `ThreadPool`; putting the registry
  //    anywhere else would let two sessions with different stores resolve to
  //    the same canonical key.
  //  * `ClientSession::handleSubscribe` already refuses to do anything when
  //    `exec_` is null, so hanging the registry off `exec_` means the dedup
  //    path has no null case and therefore no second, untested code path.
  //    `server_` IS nullable (every `WsBridge` test runs without one).
  //
  // Owned by value: the registry outlives every session on this server and is
  // torn down with the context, which is the lifetime a shared DAG needs.
  server::SharedSubscriptionRegistry& subscriptions() noexcept { return _subs; }
  const server::SharedSubscriptionRegistry& subscriptions() const noexcept { return _subs; }

private:
  AtomicStore*    _store = nullptr;
  rt::ThreadPool* _pool  = nullptr;
  server::SharedSubscriptionRegistry _subs;
};

} // namespace gma
