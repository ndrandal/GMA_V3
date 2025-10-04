#pragma once
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace gma::rt {

class ShutdownCoordinator {
public:
  // Lower order runs earlier; higher order runs later.
  void registerStep(std::string name, int order, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mx_);
    steps_.push_back({std::move(name), order, std::move(fn)});
    sorted_ = false;
  }

  // Idempotent stop: each step executed once, in ascending order.
  void stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return; // already stopping
    }
    std::vector<Step> run;
    {
      std::lock_guard<std::mutex> lk(mx_);
      if (!sorted_) {
        std::sort(steps_.begin(), steps_.end(), [](const Step& a, const Step& b){
          return a.order < b.order;
        });
        sorted_ = true;
      }
      run = steps_;
    }
    for (auto &s : run) {
      try { s.fn(); } catch (...) { /* best-effort */ }
    }
  }

private:
  struct Step { std::string name; int order; std::function<void()> fn; };
  std::vector<Step> steps_;
  std::atomic<bool> stopping_{false};
  std::mutex mx_;
  bool sorted_{false};
};

} // namespace gma::rt
