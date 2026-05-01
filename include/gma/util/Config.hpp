#pragma once

#include <string>
#include <vector>
#include <cstdint>

#include "gma/SourceProfile.hpp"

namespace gma {
namespace util {

class Config {
public:
  // Construct with sensible defaults.
  Config() = default;

  // Load from a simple "key=value" file. Engine-known keys are parsed
  // directly; everything else is parked in a pending list and forwarded
  // to ConfigNamespaceRegistry on dispatchPendingKeys() (called after
  // connectors register their readers in registerWith).
  // Returns true if the file was read successfully (even if some keys
  // are unknown to both the engine and any registered namespace).
  bool loadFromFile(const std::string& path);

  // Replay every key parked during loadFromFile through
  // ConfigNamespaceRegistry::dispatch(). Called by the composition root
  // after each IConnector::registerWith() has had a chance to register
  // its namespaces. Returns the number of keys that found a reader; the
  // remainder are silently dropped (forward-compat for stray keys).
  std::size_t dispatchPendingKeys();

  // --- Public fields (referenced elsewhere in your code) ---
  // TA params (now actually members so those C2039 errors go away)
  int    taMACD_fast  = 12;   // fast EMA period
  int    taMACD_slow  = 26;   // slow EMA period
  int    taBBands_n   = 20;   // lookback
  double taBBands_stdK = 2.0; // width in standard deviations

  // SMA periods (default: 5, 20)
  std::vector<int> taSMA = {5, 20};

  // EMA periods (default: 12, 26)
  std::vector<int> taEMA = {12, 26};

  // RSI period (default: 14)
  int taRSI = 14;

  // ATR period (default: 14)
  int taATR = 14;

  // Momentum/ROC period (default: 10)
  int taMomentum = 10;

  // MACD signal EMA period (default: 9)
  int taMACD_signal = 9;

  // Volume average period (default: 20)
  int taVolAvg = 20;

  // Server ports
  int wsPort   = 8080;
  int feedPort = 9001;

  // Thread pool size (0 = use hardware_concurrency)
  int threadPoolSize = 0;

  // History cap for Dispatcher (per symbol+field pair)
  int taHistoryMax = 1000;

  // Maximum distinct symbols tracked before rejecting new ones.
  int maxSymbols = 10000;

  // Maximum distinct fields per symbol in per-field histories.
  int maxFieldsPerSymbol = 200;

  // Metrics reporter
  bool metricsEnabled = false;
  int  metricsIntervalSec = 15;

  // Logging
  std::string logLevel = "info";

  // External WebSocket feed URL (empty = disabled)
  // e.g. "wss://feed-sim.v3m.xyz/feed" or "ws://localhost:8100/feed"
  std::string feedUrl;

  // Symbols to subscribe to on the feed (empty or ["*"] = all)
  std::vector<std::string> feedSymbols = {"*"};

  // Allow negative prices in order book (for bonds with negative yields, spreads, etc.)
  bool allowNegativePrices = false;

  // Source profile: configurable field-name mapping for the data feed.
  // Defaults match the legacy NASDAQ-style field names.
  // Used by Dispatcher for the TCP FeedServer path.
  gma::SourceProfile sourceProfile;

  // Per-feed configuration for external WebSocket feeds.
  struct FeedConfig {
      std::string url;
      std::string adapter = "itch";       // "itch" or "generic"
      std::vector<std::string> symbols = {"*"};
  };
  std::vector<FeedConfig> feeds;

private:
  static bool parseLineKV(const std::string& line, std::string& k, std::string& v);
  static std::string trim(const std::string& s);

  // Parked unknown-to-engine keys awaiting dispatchPendingKeys().
  std::vector<std::pair<std::string, std::string>> _pendingKeys;
};

} // namespace util
} // namespace gma
