#pragma once
#include <cstddef>
#include <utility>
#include <vector>

namespace gma::ob {

// Forward decls from your OB model
struct Level {
  double px{};
  double qty{};
};

struct Ladder {
  // Primary storage
  std::vector<Level> levels;

  // Convenience (lets existing code treat Ladder like a vector)
  size_t size() const { return levels.size(); }
  bool   empty() const { return levels.empty(); }
  Level& operator[](size_t i) { return levels[i]; }
  const Level& operator[](size_t i) const { return levels[i]; }
  const Level& front() const { return levels.front(); }
  auto begin()       { return levels.begin(); }
  auto end()         { return levels.end(); }
  auto begin() const { return levels.begin(); }
  auto end()   const { return levels.end(); }
};

enum class Side { Bid, Ask };

struct ObSnapshot {
  Ladder bids;
  Ladder asks;
};

enum class Metric {
  LevelPx,
  RangePx,
  Imbalance,
  Spread,
  Mid
};

// Specs used by keys / callers
struct LevelPxSpec {
  Side side{Side::Bid};
  size_t index{0};
};

struct RangePxSpec {
  Side side{Side::Bid};
  size_t lo{0}; // inclusive
  size_t hi{0}; // exclusive
};

// For imbalance-by-levels calls that previously passed a “Range” type
struct RangeSpec {
  Side side{Side::Bid};
  size_t levels{1};
};

// The “key” that selects what to compute
struct ObKey {
  Metric metric{Metric::Mid};

  // Filled depending on metric:
  LevelPxSpec levelPx{};
  RangePxSpec rangePx{};
  bool imbByLevels{true};
  size_t imbLv{1};         // if imbByLevels==true
  size_t imbP1{0}, imbP2{0}; // if imbByLevels==false: [lo, hi) in levels on the chosen side

  // ... any other fields you keep (symbol, etc.)
};

class Materializer {
public:
  // --- Public “metric” surface used by switch(ok.metric) ---
  static double levelPx(const ObSnapshot& snap, LevelPxSpec spec, double tick);
  static double rangePxReduce(const ObSnapshot& snap, RangePxSpec spec, double tick);
  static double imbalanceLevels(const ObSnapshot& snap, RangeSpec spec);
  static double spreadPx(const ObSnapshot& snap, double tick);
  static double midPx(const ObSnapshot& snap, double tick);

  // --- Overloads that some older call-sites may expect (what your errors showed) ---
  static double levelPx(const ObSnapshot& snap, size_t levelIdx, double tick); // assumes best *ask* if you keep calling this; better use LevelPxSpec
  static double rangePxReduce(const ObSnapshot& snap, std::pair<size_t,size_t> levelsHalfOpen, double tick); // assumes *ask* side
  static double imbalanceLevels(const ObSnapshot& snap, size_t levels); // assumes *ask* side

  // Utility used by a dispatcher
  static double evaluate(const ObSnapshot& snap, const ObKey& ok, double tick);

private:
  static const Ladder& bySide(const ObSnapshot& s, Side sd);
  static double clampLevelPx(const Ladder& L, size_t idx);
  static std::pair<size_t,size_t> clampRange(const Ladder& L, size_t lo, size_t hi);
};

} // namespace gma::ob
