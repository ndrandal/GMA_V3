#pragma once

#include <string>
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

  // Add any other config knobs you need here…

private:
  static bool parseLineKV(const std::string& line, std::string& k, std::string& v);
  static std::string trim(const std::string& s);
};

} // namespace util
} // namespace gma
