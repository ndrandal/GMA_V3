#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace gma {
namespace util {

class MetricRegistry {
public:
  MetricRegistry() = default;
  ~MetricRegistry();

  // Start/stop a background reporter that prints counters every N seconds.
  // (Non-static; matches how we use instance state like thr_.)
  void startReporter(unsigned int intervalSeconds = 10);
  void stopReporter();

  // Simple API: increment a named counter or set a gauge.
  void increment(const std::string& name, double v = 1.0);
  void setGauge(const std::string& name, double v);

private:
  void reporterLoop(unsigned int intervalSeconds);

  std::mutex mu_;
  std::unordered_map<std::string, double> counters_;
  std::unordered_map<std::string, double> gauges_;

  std::atomic<bool> running_{false};
  std::thread thr_;
};

} // namespace util
} // namespace gma
