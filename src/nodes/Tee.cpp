#include "gma/nodes/Tee.hpp"

namespace gma {

Tee::Tee(std::vector<std::shared_ptr<INode>> outputs)
  : outputs_(std::move(outputs)) {}

void Tee::onValue(const StreamValue& sv) {
  // Early-out on stopping_ is an optimization; correctness is guaranteed by the
  // mutex snapshot below: if shutdown() races, we observe an emptied vector.
  if (stopping_.load(std::memory_order_acquire)) return;

  // Snapshot the outputs under the lock, then forward outside it so the mutex
  // is never held during a downstream onValue() (matches Aggregate/Worker).
  std::vector<std::shared_ptr<INode>> outs;
  {
    std::lock_guard<std::mutex> lk(mx_);
    outs = outputs_;
  }

  // Each output receives the same value, unchanged — this is the fan-out.
  for (const auto& o : outs) {
    if (o) o->onValue(sv);
  }
}

void Tee::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);

  std::vector<std::shared_ptr<INode>> outs;
  {
    std::lock_guard<std::mutex> lk(mx_);
    outs.swap(outputs_);
  }

  // Propagate to exclusively-owned outputs outside the lock. shutdown() is
  // noexcept and idempotent on every node, so this is safe even when an
  // external keepAlive list also shuts these nodes down.
  for (auto& o : outs) {
    if (o) o->shutdown();
  }
}

std::size_t Tee::size() const noexcept {
  std::lock_guard<std::mutex> lk(mx_);
  return outputs_.size();
}

} // namespace gma
