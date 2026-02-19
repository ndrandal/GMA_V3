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
class Interval final : public INode,
                       public std::enable_shared_from_this<Interval> {
public:
  Interval(std::chrono::milliseconds period,
           std::shared_ptr<INode> child,
           gma::rt::ThreadPool* pool);

  ~Interval();

  // Must be called after construction when owned by a shared_ptr.
  void start();

  void onValue(const SymbolValue&) override; // no-op (source node)
  void shutdown() noexcept override;

private:
  void timerLoop();

  const std::chrono::milliseconds period_;
  std::weak_ptr<INode> child_;
  gma::rt::ThreadPool* pool_;

  std::atomic<bool> stopping_{false};
  std::atomic<bool> started_{false};

  std::mutex mx_;
  std::condition_variable cv_;
  std::thread timerThread_;
};

} // namespace gma
