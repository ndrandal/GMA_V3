#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"

namespace gma {

// Wall-clock-aligned periodic tick source.
//
// Differs from gma::Interval in one critical way: BucketTime emits each
// tick on the wall-clock period boundary rather than `period` after the
// previous tick. With period=60000 (one minute), ticks fire at
// 00:00:00, 00:01:00, 00:02:00, ... regardless of when the node was
// constructed. With period=1000, ticks fire at the .000 of each
// second.
//
// Why this exists: GMA pipelines that produce 1-minute OHLC candles
// or 1-minute volume buckets need every consumer to bucket on the
// same boundary so a "1m bar" means the same wall-clock window across
// the data plane. Using Interval(60000ms) here would drift relative
// to the wall clock — clients connecting at different moments would
// receive slightly-shifted buckets. BucketTime guarantees alignment.
//
// The ThreadPool / shutdown / shared_from_this contract matches
// Interval (see Interval.hpp). shutdown() is synchronous; the timer
// thread is joined (or detached safely if shutdown is called from the
// timer thread itself).
class BucketTime final : public INode,
                         public std::enable_shared_from_this<BucketTime> {
public:
  BucketTime(std::chrono::milliseconds period,
             std::shared_ptr<INode> child,
             gma::rt::ThreadPool* pool);

  ~BucketTime();

  // Must be called after construction when owned by a shared_ptr.
  void start();

  void onValue(const StreamValue&) override; // no-op (source node)
  void shutdown() noexcept override;

  // Returns the next wall-clock instant aligned to `period` that lies
  // strictly after `from` (when `from` is itself on a boundary, the
  // result is the *next* boundary, never `from`). Public for direct
  // testing — no state, pure function of the inputs.
  static std::chrono::system_clock::time_point nextAlignedAfter(
      std::chrono::system_clock::time_point from,
      std::chrono::milliseconds period);

private:
  void timerLoop();

  const std::chrono::milliseconds period_;
  std::shared_ptr<INode> child_;
  gma::rt::ThreadPool* pool_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};

  std::mutex mx_;
  std::condition_variable cv_;
  std::thread timerThread_;
};

} // namespace gma
