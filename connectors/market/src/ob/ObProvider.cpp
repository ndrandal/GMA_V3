#include "gma/ob/ObProvider.hpp"
#include "gma/ob/ObMaterializer.hpp"  // eval(Snapshot, ObKey)
#include "gma/ob/ObKey.hpp"           // parseObKey, ObKey, Metric, Mode

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace gma::ob {
namespace {

// Depth hint for price-band / price-keyed queries whose required ladder depth
// is not bounded by an explicit level index. The capture source treats this as
// a hint; bands deeper than this are truncated.
constexpr std::size_t kPriceBandLevels = 256;

// How many ladder levels the materializer needs to evaluate `k`. Index-based
// metrics bound their own depth; price-band / meta metrics fall back to a
// generous default since their depth isn't expressed as a level index.
std::size_t neededLevels(const ObKey& k) {
  switch (k.metric) {
    case Metric::Best:
    case Metric::Spread:
    case Metric::Mid:
      return 1;
    case Metric::LevelIdx:
      return static_cast<std::size_t>(std::max(1, k.levelIdx.n));
    case Metric::Cum:
      return static_cast<std::size_t>(std::max(1, k.cumN));
    case Metric::RangeIdx:
      return static_cast<std::size_t>(std::max(1, k.rangeIdx.lv.b));
    case Metric::VWAP:
      return k.vwapByLevels
                 ? static_cast<std::size_t>(std::max(1, k.vwapLv.b))
                 : kPriceBandLevels;
    case Metric::Imbalance:
      return k.imbByLevels
                 ? static_cast<std::size_t>(std::max(1, k.imbLv.b))
                 : kPriceBandLevels;
    case Metric::LevelPx:
    case Metric::RangePx:
      return kPriceBandLevels;
    case Metric::Meta:
      // so meta.levels.{bid,ask} report a useful (if hint-capped) level count.
      return kPriceBandLevels;
    default:
      return 1;
  }
}

} // namespace

Provider::Provider(std::shared_ptr<FunctionalSnapshotSource> src,
                   std::size_t defPerLevels,
                   std::size_t defAggLevels)
: src_(std::move(src)), defPer_(defPerLevels), defAgg_(defAggLevels) {}

double Provider::get(const std::string& symbol, const std::string& fullKey) const {
  if (!src_) return std::numeric_limits<double>::quiet_NaN();

  // Defensive: parseObKey itself is non-throwing, but the capture source is
  // user-supplied glue — keep the catch-all so a misbehaving source degrades
  // to NaN rather than propagating.
  try {
    return getImpl(symbol, fullKey);
  } catch (...) {
    return std::numeric_limits<double>::quiet_NaN();
  }
}

double Provider::getImpl(const std::string& symbol, const std::string& fullKey) const {
  // Parse the full ob.* key, then evaluate it against a freshly captured
  // snapshot via the shared materializer. This resolves EVERY advertised
  // ob.* family (best/mid/spread/level/at/range/cum/vwap/imbalance/meta),
  // not just the four the bespoke fast paths used to handle. Unparseable or
  // unknown keys yield std::nullopt -> NaN.
  auto parsed = parseObKey(fullKey);
  if (!parsed) return std::numeric_limits<double>::quiet_NaN();
  const ObKey& k = *parsed;

  // Capture enough depth for this key (always at least the per-level default,
  // matching the previous fast-path behavior of max(defPer_, N)).
  const std::size_t depth = std::max<std::size_t>(defPer_, neededLevels(k));
  Snapshot snap = src_->capture(symbol, depth, k.mode, std::nullopt);
  return eval(snap, k);
}

} // namespace gma::ob
