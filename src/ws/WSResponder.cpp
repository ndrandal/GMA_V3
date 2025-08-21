#include "gma/ws/WSResponder.hpp"
#include <sstream>
#include <chrono>
#include <iomanip>

namespace gma::ws {

static inline int64_t now_ms(){
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void WSResponder::onValue(const SymbolValue& v) {
  // Small, dependency-free JSON writer
  std::ostringstream oss;
  oss << "{\"type\":\"update\",\"id\":\"" << reqId_
      << "\",\"symbol\":\"" << v.symbol
      << "\",\"value\":" << std::setprecision(15) << v.value
      << ",\"ts\":" << now_ms() << "}";

  try { send_(oss.str()); } catch (...) { /* optional: log */ }
}

} // namespace gma::ws
