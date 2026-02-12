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
  if (!started_.compare_exchange_strong(expected, true))
    return; // already started

  timerThread_ = std::thread([self = shared_from_this()] {
    self->timerLoop();
  });
}

void Interval::timerLoop() {
  while (true) {
    {
      std::unique_lock<std::mutex> lk(mx_);
      if (cv_.wait_for(lk, period_, [this] {
            return stopping_.load(std::memory_order_acquire);
          })) {
        break; // woken by shutdown
      }
    }

    if (stopping_.load(std::memory_order_acquire))
      break;

    auto c = child_.lock();
    if (!c) break; // child gone, stop ticking

    if (pool_) {
      pool_->post([c] { c->onValue(SymbolValue{"", 0.0}); });
    } else {
      c->onValue(SymbolValue{"", 0.0});
    }
  }
}

void Interval::onValue(const SymbolValue&) {
  // source node: no upstream input
}

void Interval::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) timerThread_.join();
}

} // namespace gma
