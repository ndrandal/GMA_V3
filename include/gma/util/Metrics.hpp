#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace gma {
namespace util {

// A very small, thread-safe in-process metrics registry.
// - Counters are "add-only" numbers.
// - Gauges are "set" numbers.
// Optional: can run a background reporter thread.
class MetricRegistry {
public:
  static MetricRegistry& instance() {
    static MetricRegistry inst;
    return inst;
  }

  MetricRegistry() = default;

  // Stop reporter on shutdown (best effort).
  ~MetricRegistry() { stopReporter(); }

  MetricRegistry(const MetricRegistry&)            = delete;
  MetricRegistry& operator=(const MetricRegistry&) = delete;
  MetricRegistry(MetricRegistry&&)                 = delete;
  MetricRegistry& operator=(MetricRegistry&&)      = delete;

  // Start/stop a background reporter that prints counters every N seconds.
  void startReporter(unsigned int intervalSeconds = 10) {
    if (intervalSeconds == 0) intervalSeconds = 1;

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      return; // already running
    }

    thr_ = std::thread([this, intervalSeconds]() { reporterLoop(intervalSeconds); });
  }

  void stopReporter() {
    bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning) return;

    if (thr_.joinable()) {
      thr_.join();
    }
  }

  // Simple API: increment a named counter or set a gauge.
  void increment(const std::string& name, double v = 1.0) {
    std::lock_guard<std::mutex> lk(mu_);
    counters_[name] += v;
  }

  void setGauge(const std::string& name, double v) {
    std::lock_guard<std::mutex> lk(mu_);
    gauges_[name] = v;
  }

  // Snapshots (cheap copies) for debug/admin endpoints.
  std::unordered_map<std::string, double> snapshotCounters() const {
    std::lock_guard<std::mutex> lk(mu_);
    return counters_;
  }

  std::unordered_map<std::string, double> snapshotGauges() const {
    std::lock_guard<std::mutex> lk(mu_);
    return gauges_;
  }

private:
  void reporterLoop(unsigned int intervalSeconds) {
    using namespace std::chrono_literals;

    while (running_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
      if (!running_.load(std::memory_order_acquire)) break;

      // NOTE: We intentionally do not print here to avoid forcing iostream
      // into every TU. If you want printing, add a .cpp or include <iostream>.
      // Typical usage: call snapshotCounters() from your own logger.
    }
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, double> counters_;
  std::unordered_map<std::string, double> gauges_;

  std::atomic<bool> running_{false};
  std::thread thr_;
};

} // namespace util

// -----------------------------------------------------------------------------
// OrderBookManager-local metrics (what your OrderBookManager.hpp expects)
// -----------------------------------------------------------------------------

struct MetricsSnapshot {
  uint64_t adds             = 0;
  uint64_t updates          = 0;
  uint64_t deletes          = 0;
  uint64_t trades           = 0;

  uint64_t priorities       = 0;   // priority updates
  uint64_t snapshots        = 0;   // snapshot apply calls
  uint64_t summaries        = 0;   // level summaries
  uint64_t deltasPublished  = 0;   // event bus publishes

  uint64_t droppedStale     = 0;
  uint64_t droppedMalformed = 0;

  uint64_t seqGaps          = 0;
  uint64_t seqResets        = 0;   // onReset / epoch reset events
  uint64_t staleTransitions = 0;
};

class Metrics {
public:
  // Core book actions
  void incAdds()      { adds_.fetch_add(1, std::memory_order_relaxed); }
  void incUpdates()   { updates_.fetch_add(1, std::memory_order_relaxed); }
  void incDeletes()   { deletes_.fetch_add(1, std::memory_order_relaxed); }
  void incTrades()    { trades_.fetch_add(1, std::memory_order_relaxed); }

  // Book-manager features used in src/book/OrderBookManager.cpp
  void incPriorities()      { priorities_.fetch_add(1, std::memory_order_relaxed); }
  void incSnapshots()       { snapshots_.fetch_add(1, std::memory_order_relaxed); }
  void incSummaries()       { summaries_.fetch_add(1, std::memory_order_relaxed); }
  void incDeltasPublished() { deltasPublished_.fetch_add(1, std::memory_order_relaxed); }

  // Gating / hygiene
  void incDroppedStale()     { droppedStale_.fetch_add(1, std::memory_order_relaxed); }
  void incDroppedMalformed() { droppedMalformed_.fetch_add(1, std::memory_order_relaxed); }

  // Sequencing
  void incSeqGap()   { seqGaps_.fetch_add(1, std::memory_order_relaxed); }
  void incSeqReset() { seqResets_.fetch_add(1, std::memory_order_relaxed); }

  // State transitions
  void incStaleTransition() { staleTransitions_.fetch_add(1, std::memory_order_relaxed); }

  MetricsSnapshot snapshot() const {
    MetricsSnapshot s;
    s.adds             = adds_.load(std::memory_order_relaxed);
    s.updates          = updates_.load(std::memory_order_relaxed);
    s.deletes          = deletes_.load(std::memory_order_relaxed);
    s.trades           = trades_.load(std::memory_order_relaxed);

    s.priorities       = priorities_.load(std::memory_order_relaxed);
    s.snapshots        = snapshots_.load(std::memory_order_relaxed);
    s.summaries        = summaries_.load(std::memory_order_relaxed);
    s.deltasPublished  = deltasPublished_.load(std::memory_order_relaxed);

    s.droppedStale     = droppedStale_.load(std::memory_order_relaxed);
    s.droppedMalformed = droppedMalformed_.load(std::memory_order_relaxed);

    s.seqGaps          = seqGaps_.load(std::memory_order_relaxed);
    s.seqResets        = seqResets_.load(std::memory_order_relaxed);
    s.staleTransitions = staleTransitions_.load(std::memory_order_relaxed);
    return s;
  }

private:
  std::atomic<uint64_t> adds_{0};
  std::atomic<uint64_t> updates_{0};
  std::atomic<uint64_t> deletes_{0};
  std::atomic<uint64_t> trades_{0};

  std::atomic<uint64_t> priorities_{0};
  std::atomic<uint64_t> snapshots_{0};
  std::atomic<uint64_t> summaries_{0};
  std::atomic<uint64_t> deltasPublished_{0};

  std::atomic<uint64_t> droppedStale_{0};
  std::atomic<uint64_t> droppedMalformed_{0};

  std::atomic<uint64_t> seqGaps_{0};
  std::atomic<uint64_t> seqResets_{0};
  std::atomic<uint64_t> staleTransitions_{0};
};

// -----------------------------------------------------------------------------
// Optional convenience macros
// -----------------------------------------------------------------------------
#define GMA_METRIC_INC(name, d) ::gma::util::MetricRegistry::instance().increment((name), (d))
#define GMA_METRIC_HIT(name)    ::gma::util::MetricRegistry::instance().increment((name), 1.0)
#define GMA_METRIC_SET(name, v) ::gma::util::MetricRegistry::instance().setGauge((name), (v))

} // namespace gma
