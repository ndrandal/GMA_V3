#pragma once
#include <atomic>
#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <memory>
#include <vector>

namespace gma::util {

struct Counter {
  std::atomic<uint64_t> v{0};
  void inc(uint64_t d=1) { v.fetch_add(d, std::memory_order_relaxed); }
  uint64_t get() const { return v.load(std::memory_order_relaxed); }
};
struct Gauge {
  std::atomic<double> v{0.0};
  void set(double x) { v.store(x, std::memory_order_relaxed); }
  double get() const { return v.load(std::memory_order_relaxed); }
};

class MetricRegistry {
public:
  static MetricRegistry& instance();

  Counter& counter(const std::string& name);
  Gauge&   gauge(const std::string& name);

  // Reporter thread
  void startReporter(int intervalSec);
  void stopReporter();

  // Snapshot as JSON string (flat map)
  std::string snapshotJson() const;

private:
  MetricRegistry() = default;

  mutable std::mutex mx_;
  std::unordered_map<std::string, std::unique_ptr<Counter>> counters_;
  std::unordered_map<std::string, std::unique_ptr<Gauge>> gauges_;

  std::atomic<bool> stopping_{false};
  std::thread thr_;
};

} // namespace gma::util

// Shorthand macros
#define METRIC_INC(name, d) ::gma::util::MetricRegistry::instance().counter(name).inc(d)
#define METRIC_HIT(name)    ::gma::util::MetricRegistry::instance().counter(name).inc(1)
#define METRIC_SET(name, v) ::gma::util::MetricRegistry::instance().gauge(name).set(v)
