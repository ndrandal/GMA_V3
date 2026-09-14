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
// `StreamValue{symbol, std::vector<double>}` to the downstream node. Empty
// buckets are not emitted.
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
// ENC-1080 (sibling of ENC-1065, which fixed Interval) — the timer thread
// must not own the TumblingWindow. It used to capture shared_from_this(), so
// the refcount could never reach zero, ~TumblingWindow could never run, and
// dropping the last owning reference without first calling shutdown() leaked
// a live thread that went on firing into a downstream node, a ThreadPool and
// a store that had already been torn down. That failure does not surface
// where it is caused: in the ENC-1007 work the same pattern on Interval
// appeared as a SIGFPE in an unrelated suite roughly 40 suites later.
//
// Ownership now runs one way only. The thread shares a `State` block; the
// TumblingWindow owns the thread. So:
//   * dropping the last owning reference destroys the TumblingWindow, and
//     ~TumblingWindow stops and joins the timer thread — shutdown() is no
//     longer load-bearing for correctness, only for stopping early; and
//   * a thread detached by a re-entrant shutdown() keeps the State alive but
//     never the TumblingWindow, so it can still finish its loop safely.
//
// `start()` is safe to call once; later calls are no-ops. `shutdown()` is
// synchronous; the timer thread is joined (or detached safely if shutdown is
// called from the timer thread itself).
//
// Per-symbol buffer growth is capped by `MAX_SYMBOLS` (matches Worker's
// constant) to refuse unbounded map growth from pathological inputs.
class TumblingWindow final : public INode {
public:
  TumblingWindow(std::chrono::milliseconds period,
                 std::shared_ptr<INode> downstream,
                 gma::rt::ThreadPool* pool);

  ~TumblingWindow();

  // Starts the timer thread. Safe to call once; later calls are no-ops.
  void start();

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  static constexpr std::size_t MAX_SYMBOLS = 10000;

  // Everything the timer thread touches. Held by shared_ptr so that a detached
  // thread keeps it alive without keeping the TumblingWindow alive.
  struct State {
    State(std::chrono::milliseconds p,
          std::shared_ptr<INode> ds,
          gma::rt::ThreadPool* pl)
      : period(p), pool(pl), downstream(std::move(ds)) {}  // member order

    const std::chrono::milliseconds period;
    gma::rt::ThreadPool* const pool;

    std::atomic<bool> stopping{false};

    // mx guards `downstream` and the per-symbol buffer map AND the cv-based
    // wait in the timer loop. The timer flush snapshots the buffers under the
    // lock (move-out, leave empty vectors retaining capacity) and releases
    // before calling downstream — same swap-out-then-emit pattern as
    // BucketTime::timerLoop, to avoid deadlocks if downstream re-enters.
    //
    // `downstream` is guarded too (not const, unlike BucketTime's `child`):
    // shutdown() drops it so the buffers and the downstream chain are released
    // promptly, and the timer loop must not read it unsynchronized.
    std::mutex mx;
    std::condition_variable cv;
    std::shared_ptr<INode> downstream;
    std::unordered_map<std::string, std::vector<double>> acc;
  };

  static void timerLoop(const std::shared_ptr<State>& st);

  const std::shared_ptr<State> state_;
  std::atomic<bool> started_{false};
  std::thread timerThread_;
};

} // namespace gma
