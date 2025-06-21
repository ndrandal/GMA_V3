#pragma once
#include <functional>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>

namespace gma {
class ThreadPool {
public:
  ThreadPool(size_t numThreads);
  ~ThreadPool();

  void post(std::function<void()> task);
  void shutdown();

private:
  std::vector<std::thread> _workers;
  std::queue<std::function<void()>> _tasks;
  std::mutex _mutex;
  std::condition_variable _cond;
  std::atomic<bool> _stop{false};

  void workerLoop();
};
}
