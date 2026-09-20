#pragma once
#include <atomic>
#include <cstdint>
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
// ENC-1080 (sibling of ENC-1065, which fixed Interval) — the timer thread
// must not own the BucketTime. It used to capture shared_from_this(), so the
// refcount could never reach zero, ~BucketTime could never run, and dropping
// the last owning reference without first calling shutdown() leaked a live
// thread that went on firing into a child, a ThreadPool and a store that had
// already been torn down. That failure does not surface where it is caused:
// in the ENC-1007 work the same pattern on Interval appeared as a SIGFPE in
// an unrelated suite roughly 40 suites later.
//
// Ownership now runs one way only. The thread shares a `State` block; the
// BucketTime owns the thread. So:
//   * dropping the last owning reference destroys the BucketTime, and
//     ~BucketTime stops and joins the timer thread — shutdown() is no longer
//     load-bearing for correctness, only for stopping early; and
//   * a thread detached by a re-entrant shutdown() keeps the State alive but
//     never the BucketTime, so it can still finish its loop safely.
class BucketTime final : public INode {
public:
  BucketTime(std::chrono::milliseconds period,
             std::shared_ptr<INode> child,
             gma::rt::ThreadPool* pool);

  ~BucketTime();

  // Starts the timer thread. Safe to call once; later calls are no-ops.
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

  // ENC-1280 / SPEC specs/2026-09-20-timestamps-on-the-wire D9.
  //
  // Epoch-ms START of the bucket that CLOSES at `boundary`, where `boundary`
  // is a value returned by nextAlignedAfter(). That is the bucket whose
  // contents a timer node emits when it wakes at `boundary`, so this is the
  // bar identity that belongs on the emitted StreamValue.
  //
  // Derived from `boundary` by subtracting exactly one period rather than by
  // independently floor-aligning a clock reading, so it is *identical by
  // construction* to `nextAlignedAfter(...) - period` — including the
  // truncating-division behaviour that would otherwise make the two disagree
  // for pre-epoch instants. D10 forbids a wall-clock basis; this is
  // bucket-derived and therefore bucket-stable.
  //
  // Returns 0 ("no bucket identity", see StreamValue::bucketStartMs) for a
  // non-positive period — the same defensive case nextAlignedAfter handles by
  // returning `from` unaligned.
  static std::int64_t bucketStartMsFor(
      std::chrono::system_clock::time_point boundary,
      std::chrono::milliseconds period);

private:
  // Everything the timer thread touches. Held by shared_ptr so that a detached
  // thread keeps it alive without keeping the BucketTime alive.
  struct State {
    State(std::chrono::milliseconds p,
          std::shared_ptr<INode> c,
          gma::rt::ThreadPool* pl)
      : period(p), child(std::move(c)), pool(pl) {}

    const std::chrono::milliseconds period;
    const std::shared_ptr<INode> child;
    gma::rt::ThreadPool* const pool;

    std::atomic<bool> stopping{false};
    std::mutex mx;
    std::condition_variable cv;
  };

  static void timerLoop(const std::shared_ptr<State>& st);

  const std::shared_ptr<State> state_;
  std::atomic<bool> started_{false};
  std::thread timerThread_;
};

} // namespace gma
