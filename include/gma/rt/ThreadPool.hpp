#pragma once
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <functional>
#include <mutex>
#include <memory>

namespace gma::rt {

class ThreadPool {
public:
  explicit ThreadPool(unsigned nThreads = std::thread::hardware_concurrency());
  ~ThreadPool();

  void post(std::function<void()> fn);

  // optional: block until the queue is empty (not strictly needed)
  void drain();

private:
  void workerLoop();

  std::vector<std::thread> threads_;
  std::mutex mx_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> q_;
  std::atomic<bool> stopping_{false};
};

} // namespace gma::rt

// Global pool (simple & effective)
namespace gma {
  extern std::shared_ptr<gma::rt::ThreadPool> gThreadPool;
}
