// File: include/gma/rt/ThreadPool.hpp
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace gma::rt {

class ThreadPool {
public:
  explicit ThreadPool(unsigned nThreads = std::thread::hardware_concurrency());
  ~ThreadPool();

  ThreadPool(const ThreadPool&)            = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&)                 = delete;
  ThreadPool& operator=(ThreadPool&&)      = delete;

  // Enqueue work
  void post(std::function<void()> fn);

  // Best-effort drain: waits until queue is empty (does not guarantee workers are idle
  // if tasks enqueue more tasks).
  void drain();

private:
  void workerLoop();

private:
  std::vector<std::thread>           threads_;
  std::mutex                        mx_;
  std::condition_variable           cv_;
  std::queue<std::function<void()>> q_;
  std::atomic<bool>                 stopping_{false};
};

} // namespace gma::rt

// Global pool (decl only). Define it in exactly ONE .cpp.
namespace gma {
  extern std::shared_ptr<gma::rt::ThreadPool> gThreadPool;
}
