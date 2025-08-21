#include "gma/rt/ThreadPool.hpp"
#include <cassert>

namespace gma::rt {

ThreadPool::ThreadPool(unsigned nThreads) {
  if (nThreads == 0) nThreads = 1;
  threads_.reserve(nThreads);
  for (unsigned i=0;i<nThreads;++i) {
    threads_.emplace_back([this]{ workerLoop(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> lk(mx_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) if (t.joinable()) t.join();
}

void ThreadPool::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lk(mx_);
    if (stopping_) return;
    q_.push(std::move(fn));
  }
  cv_.notify_one();
}

void ThreadPool::drain() {
  std::unique_lock<std::mutex> lk(mx_);
  cv_.wait(lk, [this]{ return q_.empty(); });
}

void ThreadPool::workerLoop() {
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lk(mx_);
      cv_.wait(lk, [this]{ return stopping_ || !q_.empty(); });
      if (stopping_ && q_.empty()) return;
      fn = std::move(q_.front()); q_.pop();
    }
    try { fn(); } catch (...) { /* swallow: keep the pool alive */ }
  }
}

} // namespace gma::rt

namespace gma {
  std::shared_ptr<gma::rt::ThreadPool> gThreadPool;
}
