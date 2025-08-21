#pragma once
#include <memory>
#include <string>
#include "gma/ob/ObSnapshot.hpp"
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

// Pull/JIT provider usable by AtomicAccessor via a registry hook.
class Provider {
public:
  explicit Provider(std::shared_ptr<const SnapshotSource> src,
                    size_t defaultLevelsPer = 20,
                    size_t defaultLevelsAgg = 20)
  : src_(std::move(src)), defPer_(defaultLevelsPer), defAgg_(defaultLevelsAgg) {}

  // Returns NaN if key invalid/undefined.
  double get(const std::string& symbol, const std::string& keyStr) const;

private:
  std::shared_ptr<const SnapshotSource> src_;
  size_t defPer_, defAgg_;
};

} // namespace gma::ob
