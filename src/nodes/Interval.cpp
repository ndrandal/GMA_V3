#include "gma/nodes/Interval.hpp"

namespace gma {

Interval::Interval(std::chrono::milliseconds period,
                   std::shared_ptr<INode> child,
                   gma::rt::ThreadPool* pool)
  : period_(period), child_(std::move(child)), pool_(pool)
{
}

void Interval::start() {
  bool expected = false;
  if (scheduled_.compare_exchange_strong(expected, true)) {
    auto self = shared_from_this();
    pool_->post([self]{ self->tickOnce(); });
  }
}

void Interval::tickOnce() {
  if (stopping_.load(std::memory_order_acquire)) {
    scheduled_.store(false, std::memory_order_release);
    return;
  }

  // Emit a “tick”. We use an empty symbol/value – downstream nodes that care
  // (e.g. AtomicAccessor) will ignore the input payload and pull from storage.
  if (auto c = child_.lock()) {
    c->onValue(SymbolValue{ /*symbol*/"", /*value*/ 0.0 });
  }

  // re-schedule
  auto self = shared_from_this();
  pool_->post([self]{
    std::this_thread::sleep_for(self->period_);
    self->tickOnce();
  });
}

void Interval::onValue(const SymbolValue&) {
  // source node: no upstream input
}

void Interval::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
}

} // namespace gma
