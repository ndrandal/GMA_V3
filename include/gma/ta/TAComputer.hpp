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

  // Get/create per-symbol state (non-const).
  SymState& sym(const std::string& symbol);

  // Const accessor; throws if missing to avoid accidental mutation.
  const SymState& symConst(const std::string& symbol) const;

  // Optional helpers you might already call elsewhere:
  void setLastPrice(const std::string& symbol, double px);
  double getLastPrice(const std::string& symbol) const;

private:
  // maps symbol -> state
  mutable std::mutex mx_;
  std::unordered_map<std::string, SymState> map_;
};

} // namespace gma::ta
