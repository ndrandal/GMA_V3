#pragma once
#include <cstdint>
#include <optional>

namespace gma { namespace ob {

// ---- Forward declarations of types used here (so we don't drag heavy headers) ----
struct ObKey;              // defined elsewhere in your ob module
class  ObEngine;           // order book engine providing snapshots/ticks

// Simple specs/types referenced in your .cpp (names taken from error log)
struct LevelPx {
  enum class Side : uint8_t { Bid = 0, Ask = 1 };
  Side side{Side::Bid};
  int  level{0};           // 0 = best, 1 = L2, etc.
};

struct RangePxSpec {
  // percentage offsets from mid or best (semantics implemented in .cpp)
  double pct1{0.0};
  double pct2{0.0};
};

struct Range {
  int from{0};             // inclusive
  int to{0};               // inclusive
};

// ---- Materializer interface expected by your .cpp ----
class Materializer {
public:
  explicit Materializer(ObEngine& engine) noexcept : src_(&engine) {}

  // Functions/signatures as used by ObMaterializer.cpp per compiler errors:
  double levelPx(int uptoLevels, LevelPx spec, const double midHint);
  double rangePxReduce(int uptoLevels, RangePxSpec spec, const double midHint);
  double spreadPx(int uptoLevels, const double midHint);
  double midPx(int uptoLevels, const double fallback);
  int    imbalanceLevels(int uptoLevels, Range r);
  double imbalanceBand(int uptoLevels, double pct1, double pct2, const double midHint);
  ObKey  eval(int uptoLevels, const ObKey& key);

private:
  ObEngine* src_; // non-owning; engine outlives materializer
};

}} // namespace gma::ob
