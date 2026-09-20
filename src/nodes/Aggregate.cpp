#include "gma/nodes/Aggregate.hpp"
#include "gma/util/Logger.hpp"
#include <stdexcept>
#include <utility>

namespace gma {

Aggregate::Aggregate(std::size_t arity, std::shared_ptr<INode> parent)
  : arity_(arity), parent_(std::move(parent))
{
  if (arity_ == 0)
    throw std::invalid_argument("Aggregate: arity must be > 0");
}

void Aggregate::addPort(std::shared_ptr<INode> port) {
  ports_.push_back(std::move(port));
}

// A value on the plain INode edge is NOT a join member — see the header. It is
// dropped, and said out loud once per node so the drop is never silent.
void Aggregate::onValue(const StreamValue& sv) {
  if (!warnedOnPipelineValue_.exchange(true, std::memory_order_acq_rel)) {
    gma::util::logger().log(gma::util::LogLevel::Warn,
      "Aggregate: value arrived on the pipeline edge, not on a declared input "
      "port, and was dropped. A fan-in node joins its own 'inputs'; it cannot "
      "consume an upstream value (SPEC specs/2026-09-20-gma-join-correctness "
      "D2). Logged once per node.",
      {{"symbol", sv.symbol}});
  }
}

void Aggregate::onPortValue(std::size_t portIndex, const StreamValue& sv) {
  // Early-out on stopping_ is an optimization; correctness is guaranteed by
  // the mutex: if shutdown() races, the lock ensures we see parent_==nullptr.
  if (stopping_.load(std::memory_order_acquire)) return;

  // Defensive: the builder never hands out an index >= arity_ (it constructs
  // exactly one port per declared input and refuses a request whose `arity`
  // disagrees with `inputs.size()`), so this is unreachable from client JSON.
  if (portIndex >= arity_) return;

  std::vector<ArgType> batch;
  std::shared_ptr<INode> p;
  {
    std::lock_guard<std::mutex> lk(mx_);
    // Cap distinct symbol count to prevent unbounded map growth.
    auto it = buf_.find(sv.symbol);
    if (it == buf_.end()) {
      if (buf_.size() >= MAX_SYMBOLS) {
        gma::util::logger().log(gma::util::LogLevel::Warn,
          "Aggregate: max symbols reached, dropping",
          {{"symbol", sv.symbol}});
        return;
      }
      it = buf_.emplace(sv.symbol, SymBuf{}).first;
      it->second.slots.resize(arity_);
    }

    auto& sb = it->second;
    if (!sb.slots[portIndex].has_value()) ++sb.filled;
    sb.slots[portIndex] = sv.value;          // last-value-wins within the tuple

    if (sb.filled < arity_) return;          // tuple not complete yet

    // Complete. Emit in DECLARED PORT ORDER, then reset the barrier.
    batch.reserve(arity_);
    for (std::size_t i = 0; i < arity_; ++i)
      batch.push_back(std::move(*sb.slots[i]));
    for (auto& s : sb.slots) s.reset();
    sb.filled = 0;

    p = parent_;
  }

  // Forward outside the lock to avoid holding it during downstream calls.
  if (p) {
    for (auto& v : batch) {
      // ENC-1280: the tuple is released by `sv`, so every value in it is
      // forwarded under `sv`'s bucket identity.
      // ENC-1291 / SPEC Q5: still one onValue per member, deliberately — the
      // tuple is not delivered as a unit. See the header.
      p->onValue(StreamValue{ sv.symbol, std::move(v), sv.bucketStartMs });
    }
  }
}

void Aggregate::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);

  std::vector<std::shared_ptr<INode>> ports;
  {
    std::lock_guard<std::mutex> lk(mx_);
    buf_.clear();
    parent_.reset();
    ports.swap(ports_);
  }
  for (auto& p : ports)
    if (p) p->shutdown();
}

} // namespace gma
