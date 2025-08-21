#pragma once
#include <functional>
#include <optional>
#include <utility>
#include <string>
#include "gma/ob/ObSnapshot.hpp"

namespace gma::ob {

// Normalize “get me a snapshot” to simple lambdas so we don’t depend on your OB types.
class FunctionalSnapshotSource : public SnapshotSource {
public:
  // NOTE: maxLevels is a hint; if priceBand is present, you may ignore maxLevels and scan the band.
  using CaptureFn = std::function<Snapshot(const std::string& symbol,
                                           size_t maxLevels,
                                           Mode mode,
                                           std::optional<std::pair<double,double>> priceBand)>;
  using TickFn    = std::function<double(const std::string& symbol)>;

  FunctionalSnapshotSource(CaptureFn cap, TickFn tick)
  : cap_(std::move(cap)), tick_(std::move(tick)) {}

  Snapshot capture(const std::string& sym,
                   size_t n,
                   Mode m,
                   std::optional<std::pair<double,double>> band) const override {
    return cap_(sym, n, m, std::move(band));
  }
  double tickSize(const std::string& sym) const override { return tick_(sym); }

private:
  CaptureFn cap_;
  TickFn tick_;
};

} // namespace gma::ob
