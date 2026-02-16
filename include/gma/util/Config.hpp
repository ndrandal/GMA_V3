#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace gma {
namespace util {

class Config {
public:
  // Construct with sensible defaults.
  Config() = default;

  // Load from a simple "key=value" file (unknown keys ignored).
  // Returns true if file read successfully (even if some keys are unknown).
  bool loadFromFile(const std::string& path);

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

private:
  static bool parseLineKV(const std::string& line, std::string& k, std::string& v);
  static std::string trim(const std::string& s);
};

} // namespace util
} // namespace gma
