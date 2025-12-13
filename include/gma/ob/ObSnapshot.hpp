#pragma once
#include <vector>
#include <string>
#include <optional>
#include <limits>   // <-- ADD THIS
#include <cstdint>
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

// One price level
struct Level {
  double price = 0.0;
  double size  = 0.0;     // total size at this price
  double orders = std::numeric_limits<double>::quiet_NaN(); // optional (NaN if unknown)
  double notional = std::numeric_limits<double>::quiet_NaN(); // optional cached price*size
};

struct Ladder { // ordered best→worse
  std::vector<Level> levels;
};

struct Meta {
  uint64_t seq = 0;
  uint32_t epoch = 0;
  bool stale = false;
  size_t bidLevels = 0;
  size_t askLevels = 0;
  int64_t lastChangeMs = 0; // unix ms
};

struct Snapshot {
  Ladder bids, asks;
  Meta   meta;
};

// Abstract source (you implement the glue to your order book once)
struct SnapshotSource {
  virtual ~SnapshotSource() = default;
  // Capture a view. If priceBand is set, implementors may optimize by scanning that band only.
  virtual Snapshot capture(const std::string& symbol,
                           size_t maxLevels,
                           Mode mode,
                           std::optional<std::pair<double,double>> priceBand = std::nullopt) const = 0;
  virtual double tickSize(const std::string& symbol) const = 0;
};

} // namespace gma::ob
