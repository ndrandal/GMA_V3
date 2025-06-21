#include "gma/ThreadPool.hpp"

using namespace gma;

ThreadPool::ThreadPool(size_t numThreads) {
  for (size_t i = 0; i < numThreads; ++i) {
    _workers.emplace_back(&ThreadPool::workerLoop, this);
  }
}

ThreadPool::~ThreadPool() {
  shutdown();
}

void ThreadPool::post(std::function<void()> task) {
  {
    std::unique_lock lock(_mutex);
    if (_stop) return;
    _tasks.push(std::move(task));
  }
  _cond.notify_one();
}

void ThreadPool::shutdown() {
  {
    std::unique_lock lock(_mutex);
    _stop = true;
  }
  _cond.notify_all();
  for (auto& w : _workers) {
    if (w.joinable()) w.join();
  }
}

void ThreadPool::workerLoop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock lock(_mutex);
      _cond.wait(lock, [this] { return _stop || !_tasks.empty(); });
      if (_stop && _tasks.empty()) break;
      task = std::move(_tasks.front());
      _tasks.pop();
    }
    task();
  }
}
