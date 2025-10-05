#include "gma/util/Metrics.hpp"

#include <chrono>
#include <iostream>

namespace gma {
namespace util {

MetricRegistry::~MetricRegistry() {
  stopReporter();
}

void MetricRegistry::startReporter(unsigned int intervalSeconds) {
  // If already running, restart with new interval.
  stopReporter();

  running_.store(true, std::memory_order_release);
  thr_ = std::thread([this, intervalSeconds]{
    reporterLoop(intervalSeconds);
  });
}

void MetricRegistry::stopReporter() {
  bool expected = true;
  if (running_.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
    // We flipped from true->false and need to join.
    if (thr_.joinable()) thr_.join();
  } else {
    // Already stopped; still join if thread exists to be safe.
    if (thr_.joinable()) thr_.join();
  }
}

void MetricRegistry::increment(const std::string& name, double v) {
  std::lock_guard<std::mutex> lk(mu_);
  counters_[name] += v;
}

void MetricRegistry::setGauge(const std::string& name, double v) {
  std::lock_guard<std::mutex> lk(mu_);
  gauges_[name] = v;
}

void MetricRegistry::reporterLoop(unsigned int intervalSeconds) {
  using namespace std::chrono;
  auto sleep_dur = seconds(intervalSeconds > 0 ? intervalSeconds : 10);

  while (running_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(sleep_dur);

    // Snapshot under lock
    std::unordered_map<std::string, double> c;
    std::unordered_map<std::string, double> g;
    {
      std::lock_guard<std::mutex> lk(mu_);
      c = counters_;
      g = gauges_;
      // Optionally zero the counters after emission
      counters_.clear();
    }

    // Emit (stdout for now; plug your logger here)
    if (!c.empty() || !g.empty()) {
      std::cout << "[metrics] counters:";
      for (auto& kv : c) std::cout << " " << kv.first << "=" << kv.second;
      std::cout << " | gauges:";
      for (auto& kv : g) std::cout << " " << kv.first << "=" << kv.second;
      std::cout << std::endl;
    }
  }
}

} // namespace util
} // namespace gma
