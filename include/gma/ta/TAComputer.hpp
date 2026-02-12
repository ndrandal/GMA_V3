#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

namespace gma::ta {

class TAComputer {
public:
  struct SymState {
    // Add/keep whatever fields you already use elsewhere.
    // These are placeholders so compilation won’t break.
    double lastPrice{0.0};
    uint64_t lastTs{0};
  };

  TAComputer() = default;

  // Thread-safe accessors — all work done under the lock.
  void setLastPrice(const std::string& symbol, double px);
  double getLastPrice(const std::string& symbol) const;

  // Return a copy of per-symbol state (safe: no dangling references).
  SymState getState(const std::string& symbol) const;

  // Check if symbol exists.
  bool has(const std::string& symbol) const;

private:
  mutable std::mutex mx_;
  std::unordered_map<std::string, SymState> map_;
};

} // namespace gma::ta
