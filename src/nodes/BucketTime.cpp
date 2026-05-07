#include "gma/nodes/BucketTime.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

BucketTime::BucketTime(std::chrono::milliseconds period,
                       std::shared_ptr<INode> child,
                       gma::rt::ThreadPool* pool)
  : period_(period), child_(std::move(child)), pool_(pool)
{
}

BucketTime::~BucketTime() {
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) {
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
}

void BucketTime::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return;

  timerThread_ = std::thread([self = shared_from_this()] {
    self->timerLoop();
  });
}

std::chrono::system_clock::time_point
BucketTime::nextAlignedAfter(
    std::chrono::system_clock::time_point from,
    std::chrono::milliseconds period) {
  using namespace std::chrono;
  // Compute (epoch ms / period) * period + period — the next strictly-
  // greater wall-clock multiple of `period`.
  const auto epoch_ms = duration_cast<milliseconds>(from.time_since_epoch()).count();
  const auto period_ms = period.count();
  if (period_ms <= 0) {
    // Defensive — caller shouldn't pass non-positive periods. Return
    // `from` so the caller sleeps zero and re-evaluates.
    return from;
  }
  const auto next_ms = ((epoch_ms / period_ms) + 1) * period_ms;
  return system_clock::time_point{milliseconds{next_ms}};
}

void BucketTime::timerLoop() {
  while (true) {
    if (stopping_.load(std::memory_order_acquire))
      break;

    const auto now = std::chrono::system_clock::now();
    const auto target = nextAlignedAfter(now, period_);
    {
      std::unique_lock<std::mutex> lk(mx_);
      // wait_until lets shutdown wake us early without re-arming.
      if (cv_.wait_until(lk, target, [this] {
            return stopping_.load(std::memory_order_acquire);
          })) {
        break;
      }
    }
    if (stopping_.load(std::memory_order_acquire))
      break;
    if (!child_) break;

    try {
      if (pool_) {
        auto c = child_;
        pool_->post([c] { c->onValue(StreamValue{"", 0.0}); });
      } else {
        child_->onValue(StreamValue{"", 0.0});
      }
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "BucketTime::timerLoop: onValue exception",
        {{"err", ex.what()}});
    }
  }
}

void BucketTime::onValue(const StreamValue&) {
  // source node: no upstream input
}

void BucketTime::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) {
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
}

} // namespace gma
