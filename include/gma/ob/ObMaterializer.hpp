#pragma once

#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <chrono>
#include <optional>
#include <functional>
#include <atomic>

#include "gma/ob/ObSnapshot.hpp"
#include "gma/ob/ObEngine.hpp"
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

struct MaterializeConfig {
  // Which keys to materialize. If `keysBySymbol` has an entry, that wins; otherwise use defaultKeys.
  std::vector<std::string>              defaultKeys;
  std::unordered_map<std::string, std::vector<std::string>> keysBySymbol;

  // Limits for snapshots
  size_t maxLevelsPer = 20;  // per-order snapshot depth
  size_t maxLevelsAgg = 20;  // aggregated snapshot depth

  // Periodic coalescing (ms). If 0, materialize immediately when notified.
  int    intervalMs   = 250;
};

class Materializer {
public:
  using WriteFn  = std::function<void(const std::string& symbol, const std::string& key, double value, int64_t tsMs)>;
  using NotifyFn = std::function<void(const std::string& symbol, const std::string& key)>;

  Materializer() = default;

  void setSource(ObEngine* engine) { src_ = engine; }
  void setWrite(WriteFn fn) { write_ = std::move(fn); }
  void setNotify(NotifyFn fn) { notify_ = std::move(fn); }

  // Start with a config (spawns a thread if intervalMs > 0)
  void start(const MaterializeConfig& cfg);
  void stop();

  // On incoming order book update for a symbol, compute all configured keys once.
  enum class Mode { Per, Agg };
  void onOrderBookUpdate(const std::string& symbol, Mode mode);

private:
  // helpers
  static double levelPx(const ObSnapshot& snap, size_t level, double tick);
  static double rangePxReduce(const ObSnapshot& snap, std::pair<size_t,size_t> lv, double tick);
  static double spreadPx(const ObSnapshot& snap, double tick);
  static double midPx(const ObSnapshot& snap, double tick);
  static double imbalanceLevels(const ObSnapshot& snap, size_t uptoLevels);
  static double imbalanceBand(const ObSnapshot& snap, double pct1, double pct2, double tick);
  static double eval(const ObSnapshot& snap, const ParsedObKey& keySpec);

private:
  ObEngine* src_{nullptr};
  WriteFn   write_{};
  NotifyFn  notify_{};
  MaterializeConfig cfg_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::thread       thr_;
  std::mutex        mx_;
  std::condition_variable cv_;
  bool              wake_{false};    // set true to force a pass

  struct State { std::chrono::steady_clock::time_point last{}; };
  std::unordered_map<std::string, State> perState_; // per symbol coalescing
};

} // namespace gma::ob
