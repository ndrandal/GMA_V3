// include/gma/nodes/Tee.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include "gma/nodes/INode.hpp"

namespace gma {

// Fan-out node: forwards each incoming StreamValue, unchanged, to every
// downstream output. This is the data-plane multi-downstream primitive that
// lets the request tree become a reuse DAG (let-bindings, ENC-647) and enables
// value-conditioned routing (Switch, ENC-650).
//
// Contrast with CompositeRoot (src/core/TreeBuilder.cpp): that is a
// lifecycle-only wrapper for independent source subtrees whose onValue() is a
// no-op. Tee is a true data fan-out — onValue() pushes the value to N outputs.
//
// Ownership: a Tee exclusively owns its output subtrees and propagates
// shutdown() to each (like CompositeRoot; unlike mid-pipeline Worker/Aggregate,
// which only reset their single downstream). Every node's shutdown() is
// idempotent, so double-shutdown via an external keepAlive list is safe.
//
// Thread-safety: mirrors Aggregate/Worker — a stopping_ flag plus a mutex
// guarding the outputs vector; values are always forwarded outside the lock so
// the mutex is never held during a downstream onValue().
class Tee final : public INode {
public:
  explicit Tee(std::vector<std::shared_ptr<INode>> outputs);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  // Count of live outputs (test / introspection aid). Returns 0 after shutdown.
  std::size_t size() const noexcept;

private:
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  std::vector<std::shared_ptr<INode>> outputs_;
};

} // namespace gma
