#include "gma/util/Metrics.hpp"
#include "gma/util/Logger.hpp"
#include <sstream>
#include <chrono>
#include <thread>

namespace gma::util {

MetricRegistry& MetricRegistry::instance() {
  static MetricRegistry M;
  return M;
}

Counter& MetricRegistry::counter(const std::string& name) {
  std::lock_guard<std::mutex> lk(mx_);
  auto& p = counters_[name];
  if (!p) p = std::make_unique<Counter>();
  return *p;
}

Gauge& MetricRegistry::gauge(const std::string& name) {
  std::lock_guard<std::mutex> lk(mx_);
  auto& p = gauges_[name];
  if (!p) p = std::make_unique<Gauge>();
  return *p;
}

void MetricRegistry::startReporter(unsigned periodMs) {
  stopReporter();
  stopping_.store(false, std::memory_order_relaxed);
  thr_ = std::thread([this, periodMs]{
    while (!stopping_.load(std::memory_order_relaxed)) {
      // Optional: push a snapshot to logs
      // GLOG_INFO("metrics", {{"snapshot", snapshotJson()}});
      std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
    }
  });
}

void MetricRegistry::stopReporter() {
  if (thr_.joinable()) {
    stopping_.store(true, std::memory_order_relaxed);
    thr_.join();
  }
}

std::string MetricRegistry::snapshotJson() const {
  std::lock_guard<std::mutex> lk(mx_);
  std::ostringstream oss;
  oss << "{\"counters\":{";
  bool first=true;
  for (auto& kv : counters_) {
    if (!first) oss << ","; first=false;
    oss << "\"" << kv.first << "\":" << kv.second->get();
  }
  oss << "},\"gauges\":{";
  first=true;
  for (auto& kv : gauges_) {
    if (!first) oss << ","; first=false;
    oss << "\"" << kv.first << "\":" << kv.second->get();
  }
  oss << "}}";
  return oss.str();
}

} // namespace gma::util
