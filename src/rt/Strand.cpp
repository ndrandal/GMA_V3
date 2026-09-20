#include "gma/rt/Strand.hpp"

#include "gma/util/Logger.hpp"

namespace gma::rt {

void Strand::post(std::function<void()> fn) {
  if (!fn) return;

  bool kick = false;
  {
    std::lock_guard<std::mutex> lk(mx_);
    q_.push_back(std::move(fn));
    if (!running_) {
      running_ = true;
      kick     = true;
    }
  }
  if (!kick) return;                 // an existing drainer will pick it up

  if (!pool_) {
    // No executor: run inline, still in order. This is the same fallback
    // `Listener::onValue` already had when `pool_` was null, and it keeps the
    // ordering guarantee — it just borrows the caller's thread for it.
    drain();
    return;
  }

  // `shared_from_this` keeps the strand alive for the whole drain, so the last
  // external owner may drop while work is still queued.
  pool_->post([self = shared_from_this()] { self->drain(); });
}

void Strand::drain() {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lk(mx_);
      if (q_.empty()) {
        // Clearing `running_` under the same lock that `post` takes is what
        // makes the handoff race-free: a post that appends after this point
        // sees `running_ == false` and kicks a fresh drainer, and one that
        // appends before it is still in `q_` and taken by this loop.
        running_ = false;
        return;
      }
      fn = std::move(q_.front());
      q_.pop_front();
    }
    // Outside the lock: a task may post back onto this same strand.
    try {
      fn();
    } catch (const std::exception& e) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "Strand: task exception", {{"err", e.what()}});
    } catch (...) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "Strand: unknown task exception");
    }
  }
}

std::size_t Strand::queueDepth() const {
  std::lock_guard<std::mutex> lk(mx_);
  return q_.size();
}

} // namespace gma::rt
