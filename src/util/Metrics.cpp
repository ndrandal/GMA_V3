#include "gma/util/Metrics.hpp"
#include "gma/util/Logger.hpp"
#include <sstream>
#include <chrono>

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

void MetricRegistry::startReporter(int intervalSec) {
  stopReporter();
  stopping_.store(false);
  thr_ = std::thread([this, intervalSec]{
    using namespace std::chrono;
    while (!stopping_.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
      if (stopping_.load()) break;
      GLOG_INFO("metrics", {{"snapshot", snapshotJson()}});
    }
  });
}

void MetricRegistry::stopReporter() {
  stopping_.store(true);
  if (thr_.joinable()) thr_.join();
}

std::string MetricRegistry::snapshotJson() const {
  std::lock_guard<std::mutex> lk(mx_);
  std::ostringstream oss;
  oss << "{";
  bool first=true;
  for (auto& kv : counters_) {
    if (!first) oss << ","; first=false;
    oss << "\"" << kv.first << "\":" << kv.second->get();
  }
  for (auto& kv : gauges_) {
    if (!first) oss << ","; first=false;
    oss << "\"" << kv.first << "\":" << kv.second->get();
  }
  oss << "}";
  return oss.str();
}

} // namespace gma::util
