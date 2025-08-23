#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <functional>
#include "gma/ob/ObSnapshot.hpp"
#include "gma/ob/ObEngine.hpp"
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

struct MaterializeConfig {
  std::unordered_map<std::string, std::vector<std::string>> keysBySymbol;
  std::vector<std::string> defaultKeys;
  size_t maxLevelsPer = 20, maxLevelsAgg = 20;
  int throttleMs = 10;
  bool notifyOnWrite = true;
};

// Store writer + notifier are function callbacks so you can plug your existing systems
using StoreWriteFn = std::function<void(const std::string& symbol, const std::string& key, double value, int64_t ts_ms)>;
using NotifyFn     = std::function<void(const std::string& symbol, const std::string& key)>;

class Materializer {
public:
  Materializer(std::shared_ptr<const SnapshotSource> source,
               StoreWriteFn writeFn,
               std::optional<NotifyFn> notifyFn = std::nullopt)
  : src_(std::move(source)), write_(std::move(writeFn)), notify_(std::move(notifyFn)) {}

  void start(const MaterializeConfig& cfg);
  void stop();

  // Call this from your OB pipeline when a symbol's book changes.
  void onOrderBookUpdate(const std::string& symbol, Mode mode);

private:
  std::shared_ptr<const SnapshotSource> src_;
  StoreWriteFn write_;
  std::optional<NotifyFn> notify_;

  MaterializeConfig cfg_;
  std::mutex mx_;
  bool running_ = false;


  // in ObMaterializer.hpp
  std::atomic<bool> stopping_{false};
  std::thread       thr_;
  std::mutex        mx_;
  std::condition_variable cv_;
  bool              wake_{false};    // set true to force a pass


  struct State {
    std::chrono::steady_clock::time_point last{};
  };
  std::unordered_map<std::string, State> perState_; // per symbol coalescing
};

} // namespace gma::ob
