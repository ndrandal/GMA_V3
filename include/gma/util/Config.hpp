#pragma once
#include <string>
#include <vector>
#include <optional>

namespace gma::util {

struct Config {
  // Core
  int   wsPort              = 9002;
  int   threadPoolSize      = 8;
  int   listenerQueueCap    = 1024;

  // Logging
  std::string logLevel      = "info";   // trace|debug|info|warn|error
  std::string logFormat     = "json";   // json|text
  std::string logFile;                  // "" => stdout

  // Metrics
  bool  metricsEnabled      = true;
  int   metricsIntervalSec  = 10;

  // TA (from T3) — optional overrides
  int   taHistoryMax        = 4096;
  std::vector<int> taSMA    = {5,10,20,50};
  std::vector<int> taEMA    = {10,20};
  std::vector<int> taVWAP   = {10,50};
  std::vector<int> taMED    = {5,21};
  std::vector<int> taMIN    = {10};
  std::vector<int> taMAX    = {10};
  std::vector<int> taSTD    = {20};
  int   taRSI               = 14;

  static Config& get();                          // singleton
  static void   loadFromEnv(Config& out);
  static bool   loadFromFile(Config& out, const std::string& path, std::string* err = nullptr);

  // Utility
  static std::optional<std::string> env(const char* name);
  static int  envInt(const char* name, int def);
};

} // namespace gma::util
