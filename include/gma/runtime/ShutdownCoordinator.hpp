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
  // Lower order stops earlier. You can use enums (0..9).
  void registerStep(std::string name, int order, std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mx_);
    steps_.push_back({std::move(name), order, std::move(fn)});
    sorted_ = false;
  }

  // Idempotent stop-all
  void stopAll() {
    bool expected=false;
    if (!stopping_.compare_exchange_strong(expected, true)) return; // already stopping

    {
      std::lock_guard<std::mutex> lk(mx_);
      if (!sorted_) {
        std::sort(steps_.begin(), steps_.end(),
          [](const Step& a, const Step& b){ return a.order < b.order; });
        sorted_ = true;
      }
    }
    // Run outside the lock in case a step needs time
    for (auto &s : steps_) {
      try { s.fn(); } catch (...) { /* swallow; best-effort */ }
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
