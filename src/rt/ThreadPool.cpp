#include "gma/rt/ThreadPool.hpp"
#include <cassert>
#include "gma/util/Logger.hpp"

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
      inFlight_.fetch_add(1, std::memory_order_acq_rel);
    }
    try {
      fn();
    } catch (const std::exception& e) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "ThreadPool: task exception", {{"err", e.what()}});
    } catch (...) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "ThreadPool: unknown task exception");
    }
    // Decrement under mx_ so the change is ordered against drain()'s predicate
    // check: a waiter in drain() either observes inFlight_==0 before parking, or
    // parks (releasing mx_) before this critical section runs and is then woken
    // by the notify below. Decrementing outside mx_ races the predicate eval and
    // loses the wakeup (the bug this fixes — ENC-787). notify after unlocking so
    // the woken thread doesn't immediately re-block on a still-held mx_.
    {
      std::lock_guard<std::mutex> lk(mx_);
      inFlight_.fetch_sub(1, std::memory_order_acq_rel);
    }
    idleCv_.notify_all();
  }
}

} // namespace gma::rt

namespace gma {
  std::shared_ptr<gma::rt::ThreadPool> gThreadPool;
}
