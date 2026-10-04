// include/gma/nodes/SharedTerminal.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include "gma/nodes/INode.hpp"

namespace gma::nodes {

// ENC-1041. The terminal of a SHARED request DAG: one computation, N
// subscribers, each with its own `Responder` and therefore its own
// `RequestKey`. `SharedSubscriptionRegistry` owns exactly one of these per
// canonical request key and hands it to `tree::buildForRequest` as the
// terminal; every connection that resolves to that key then `attach`es its own
// Responder here.
//
// WHY THIS IS NOT `Tee` (ENC-1041, and the reason a parallel node is correct
// rather than lazy). `Tee` is the reuse-DAG primitive `Let`/`Ref` and `Switch`
// build on, and three of its properties are load-bearing for those callers and
// wrong for this one:
//
//   * Its output set is FIXED at construction. A subscriber arriving on a
//     second connection five minutes later has nowhere to go.
//   * Its `shutdown()` is all-or-nothing and propagates to every output. Here
//     the whole point is that ONE subscriber leaving must leave the other N-1
//     untouched — a `Tee` cannot express "remove this output".
//   * It forwards without catching. A sink that throws aborts the fan-out loop
//     and every LATER output silently misses that value. Inside one request's
//     DAG that is one client's problem; across connections it is one client
//     starving the others, which is a new failure mode created purely by
//     sharing. So this node catches per sink.
//
// Widening `Tee` with mutable membership would hand those semantics to `Let`
// and `Switch`, which neither needs nor wants them.
//
// Thread-safety: mirrors `Tee` — a `stopping_` flag plus a mutex guarding the
// sink vector, with values forwarded on a snapshot taken OUTSIDE the lock, so
// the mutex is never held across a downstream `onValue()`.
//
// LIFETIME. This node holds its sinks by `shared_ptr` and `shutdown()`
// propagates to whatever is still attached, the same exclusive-ownership
// contract `Tee` publishes. `detach()` deliberately does NOT shut the removed
// sink down: it hands it back, so the caller can run `shutdown()` with no
// registry or terminal lock held. See `SharedSubscriptionRegistry::release`.
class SharedTerminal final : public INode {
public:
  using SubscriberId = std::uint64_t;

  // Result of `detach`. `sink` is the removed node — the CALLER shuts it down.
  struct Detached {
    std::shared_ptr<INode> sink;        // null when `found` is false
    std::size_t            remaining{0};// subscribers still attached
    bool                   found{false};
  };

  SharedTerminal() = default;

  // Add a sink. Returns its id, which is unique for the lifetime of this node
  // and never reused — so a stale id from an already-detached subscriber can
  // never detach somebody else's sink. Returns 0 and ignores the sink after
  // `shutdown()`, because there is no longer anything to deliver.
  SubscriberId attach(std::shared_ptr<INode> sink);

  // Remove the sink registered under `id`. The removed sink is returned rather
  // than shut down here; `remaining` is the number of sinks still attached,
  // read under the same lock as the removal so a caller tearing the DAG down on
  // `remaining == 0` cannot race a concurrent `attach`.
  Detached detach(SubscriberId id);

  // Live sink count (test / introspection aid). 0 after shutdown.
  std::size_t size() const noexcept;

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  SubscriberId nextId_{1};
  std::vector<std::pair<SubscriberId, std::shared_ptr<INode>>> sinks_;
};

} // namespace gma::nodes
