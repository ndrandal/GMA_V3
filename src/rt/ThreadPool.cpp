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
  idleCv_.wait(lk, [this]{ return q_.empty() && inFlight_.load(std::memory_order_acquire) == 0; });
}

void ThreadPool::shutdown() {
  drain();
  {
    std::lock_guard<std::mutex> lk(mx_);
    stopping_ = true;
  }
  cv_.notify_all();
  for (auto& t : threads_) if (t.joinable()) t.join();
}

void ThreadPool::workerLoop() {
  for (;;) {
    std::function<void()> fn;
    {
      std::unique_lock<std::mutex> lk(mx_);
      cv_.wait(lk, [this]{ return stopping_ || !q_.empty(); });
      if (stopping_ && q_.empty()) return;
      fn = std::move(q_.front()); q_.pop();
      inFlight_.fetch_add(1, std::memory_order_relaxed);
    }
    try { fn(); } catch (...) {}
    inFlight_.fetch_sub(1, std::memory_order_release);
    idleCv_.notify_all();
  }
}

} // namespace gma::rt

namespace gma {
  std::shared_ptr<gma::rt::ThreadPool> gThreadPool;
}
