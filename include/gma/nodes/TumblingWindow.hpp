#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"

namespace gma {

// Per-period accumulator: for each `(streamKey)` seen on `onValue`, push the
// incoming scalar into a per-symbol vector buffer; on every wall-clock
// boundary aligned to `period`, swap each non-empty buffer out and emit one
// `StreamValue{symbol, std::vector<double>}` to `downstream_`. Empty buckets
// are not emitted.
//
// Companion to BucketTime / BucketTime::nextAlignedAfter — the timer logic
// is the same wall-clock-aligned wait_until pattern; the difference is that
// BucketTime is a tick *source* (no upstream input, no per-symbol state),
// while TumblingWindow taps an upstream stream and emits a reduced batch.
//
// Why the wall-clock alignment matters: a 60s TumblingWindow that started
// mid-minute would otherwise drift relative to other 60s windows, so two
// charts subscribed to the same stream would receive misaligned bars.
// Aligning to wall-clock multiples of `period` keeps every consumer's
// boundaries coincident.
//
// ThreadPool / shutdown / shared_from_this contract mirrors BucketTime
// (see BucketTime.hpp). `start()` must be called after construction when
// owned by a `shared_ptr`; `shutdown()` is synchronous; the timer thread
// is joined (or detached safely if shutdown is called from the timer
// thread itself).
//
// Per-symbol buffer growth is capped by `MAX_SYMBOLS` (matches Worker's
// constant) to refuse unbounded map growth from pathological inputs.
class TumblingWindow final : public INode,
                             public std::enable_shared_from_this<TumblingWindow> {
public:
  TumblingWindow(std::chrono::milliseconds period,
                 std::shared_ptr<INode> downstream,
                 gma::rt::ThreadPool* pool);

  ~TumblingWindow();

  // Must be called after construction when owned by a shared_ptr.
  void start();

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  void timerLoop();

  static constexpr std::size_t MAX_SYMBOLS = 10000;

  const std::chrono::milliseconds period_;
  std::shared_ptr<INode> downstream_;
  gma::rt::ThreadPool* pool_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};

  // mx_ guards both the per-symbol buffer map AND the cv_-based wait in
  // the timer loop. The timer flush snapshots the buffers under the lock
  // (move-out, leave empty vectors retaining capacity) and releases before
  // calling downstream — same swap-out-then-emit pattern as
  // BucketTime::timerLoop, to avoid deadlocks if downstream re-enters.
  std::mutex mx_;
  std::condition_variable cv_;
  std::unordered_map<std::string, std::vector<double>> acc_;
  std::thread timerThread_;
};

} // namespace gma
