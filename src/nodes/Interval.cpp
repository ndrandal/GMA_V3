#include "gma/nodes/Interval.hpp"

namespace gma {

Interval::Interval(std::chrono::milliseconds period,
                   std::shared_ptr<INode> child,
                   gma::rt::ThreadPool* pool)
  : period_(period), child_(std::move(child)), pool_(pool)
{
}

Interval::~Interval() {
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) {
    // If the destructor is running from the timer thread itself (the thread
    // held the last shared_from_this() and released it on loop exit), join()
    // would deadlock.  Detach instead — the thread is about to return.
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
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
  if (timerThread_.joinable()) {
    // Mirror the destructor guard: if shutdown() is called from the timer
    // thread itself (e.g. via a downstream callback), join() would deadlock.
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
}

} // namespace gma
