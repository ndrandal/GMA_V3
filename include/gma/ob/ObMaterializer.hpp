#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <optional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "gma/ob/ObSnapshot.hpp"
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

struct MaterializeConfig {
  int intervalMs = 0;                          // <=0: immediate mode (caller triggers)
  std::size_t maxLevelsPer = 1;
  std::size_t maxLevelsAgg = 1;
  std::unordered_map<std::string, std::vector<std::string>> keysBySymbol;
  std::vector<std::string> defaultKeys;
};

// Aliases used by the .cpp
using ObSnapshot   = Snapshot;
using ParsedObKey  = ObKey;

class Materializer {
public:
  using WriteFn  = std::function<void(const std::string& symbol,
                                      const std::string& key,
                                      double value,
                                      int64_t ts)>;
  using NotifyFn = std::function<void(const std::string& symbol,
                                      const std::string& key)>;

  explicit Materializer(std::shared_ptr<SnapshotSource> src,
                        WriteFn write,
                        std::optional<NotifyFn> notify = std::nullopt)
  : src_(std::move(src)), write_(std::move(write)), notify_(std::move(notify)) {}

  void start(const MaterializeConfig& cfg);
  void stop();
  void onOrderBookUpdate(const std::string& symbol, Mode mode);

private:
  // implemented in .cpp
  static double levelPx(const ObSnapshot& snap, std::size_t level, double tick);
  static double rangePxReduce(const ObSnapshot& snap, std::pair<std::size_t,std::size_t> lv, double tick);
  static double spreadPx(const ObSnapshot& snap, double tick);
  static double midPx(const ObSnapshot& snap, double tick);
  static double imbalanceLevels(const ObSnapshot& snap, std::size_t uptoLevels);
  static double imbalanceBand(const ObSnapshot& snap, double pct1, double pct2, double tick);
  static double eval(const ObSnapshot& snap, const ParsedObKey& keySpec);

private:
  std::shared_ptr<SnapshotSource> src_;
  WriteFn write_;
  std::optional<NotifyFn> notify_;

  MaterializeConfig cfg_{};
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  bool wake_{false};
  std::mutex mx_;
  std::condition_variable cv_;
  std::thread thr_;
};

} // namespace gma::ob
