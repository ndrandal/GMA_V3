#pragma once
#include <memory>
#include <string>
#include <cstddef>
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObKey.hpp" // for Mode

namespace gma::ob {

class Provider {
public:
  Provider(std::shared_ptr<FunctionalSnapshotSource> src,
           std::size_t defPerLevels,
           std::size_t defAggLevels);

  // Resolve a single ob.* key for symbol; returns NaN if unknown/unavailable.
  double get(const std::string& symbol, const std::string& fullKey) const;

private:
  double getImpl(const std::string& symbol, const std::string& fullKey) const;

  std::shared_ptr<FunctionalSnapshotSource> src_;
  std::size_t defPer_;
  std::size_t defAgg_;
};

} // namespace gma::ob
