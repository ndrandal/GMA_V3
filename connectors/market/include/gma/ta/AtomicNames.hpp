#pragma once
#include <string>
#include <sstream>

namespace gma::ta {

inline std::string key_px_last() { return "px.last"; }
inline std::string key_px_sma(int n){
  std::ostringstream oss; oss << "px.sma." << n; return oss.str();
}
inline std::string key_px_ema(int n){
  std::ostringstream oss; oss << "px.ema." << n; return oss.str();
}
inline std::string key_px_med(int n){
  std::ostringstream oss; oss << "px.median." << n; return oss.str();
}
inline std::string key_px_min(int n){
  std::ostringstream oss; oss << "px.min." << n; return oss.str();
}
inline std::string key_px_max(int n){
  std::ostringstream oss; oss << "px.max." << n; return oss.str();
}
inline std::string key_px_std(int n){
  std::ostringstream oss; oss << "px.std." << n; return oss.str();
}
inline std::string key_px_vwap(int n){
  std::ostringstream oss; oss << "px.vwap." << n; return oss.str();
}
inline std::string key_px_rsi(int n){
  std::ostringstream oss; oss << "px.rsi." << n; return oss.str();
}

} // namespace gma::ta
