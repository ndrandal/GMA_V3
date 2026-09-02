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

// Periodic tick source. Spawns a dedicated timer thread that sleeps for
// `period` between ticks and posts each tick to the thread pool.
// shutdown() is synchronous — the timer thread is joined before returning.
//
// ENC-1065 — the timer thread must not own the Interval. It used to capture
// shared_from_this(), so the refcount could never reach zero, ~Interval could
// never run, and dropping the last owning reference without first calling
// shutdown() leaked a live thread that went on firing into a child, a
// ThreadPool and a store that had already been torn down. That failure does
// not surface where it is caused: in the ENC-1007 work it appeared as a SIGFPE
// in an unrelated suite roughly 40 suites later.
//
// Ownership now runs one way only. The thread shares a `State` block; the
// Interval owns the thread. So:
//   * dropping the last owning reference destroys the Interval, and ~Interval
//     stops and joins the timer thread — shutdown() is no longer load-bearing
//     for correctness, only for stopping early; and
//   * a thread detached by a re-entrant shutdown() keeps the State alive but
//     never the Interval, so it can still finish its loop safely.
class Interval final : public INode {
public:
  Interval(std::chrono::milliseconds period,
           std::shared_ptr<INode> child,
           gma::rt::ThreadPool* pool);

  ~Interval();

  // Starts the timer thread. Safe to call once; later calls are no-ops.
  void start();

  void onValue(const StreamValue&) override; // no-op (source node)
  void shutdown() noexcept override;

private:
  // Everything the timer thread touches. Held by shared_ptr so that a detached
  // thread keeps it alive without keeping the Interval alive.
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
