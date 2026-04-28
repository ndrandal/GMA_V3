#pragma once
#include <string>
#include <optional>
#include <cstdint>

namespace gma::ob {

// Views
enum class Mode { Per, Agg };              // per-order vs aggregated
enum class Side { Bid, Ask };

// Schema
enum class Metric {
  Best, LevelIdx, LevelPx, RangeIdx, RangePx, Cum, VWAP, Imbalance, Spread, Mid, Meta
};
enum class Reduce { Sum, Avg, Min, Max, Count, None };
enum class Target { Price, Size, Orders, Notional, None };

struct Range { int a=0, b=0; };            // inclusive, 1-based for levels

struct LevelIdx { Side side{}; int n=0; Target attr=Target::None; };
struct LevelPx  { Side side{}; double px=0.0; Target attr=Target::None; };
struct RangeIdxSpec { Side side{}; Range lv{}; Reduce reduce=Reduce::None; Target target=Target::None; };
struct RangePxSpec  { Side side{}; double p1=0, p2=0; Reduce reduce=Reduce::None; Target target=Target::None; };

struct ObKey {
  Metric metric{};
  Mode   mode = Mode::Per;

  // discriminated union fields (use the one matching metric)
  Side bestSide{}; Target bestAttr=Target::None;          // Best
  LevelIdx levelIdx{};                                     // LevelIdx
  LevelPx  levelPx{};                                      // LevelPx
  RangeIdxSpec rangeIdx{};                                 // RangeIdx
  RangePxSpec  rangePx{};                                  // RangePx
  int cumN=0; Side cumSide{}; Target cumTarget=Target::None; // Cum
  Side vwapSide{}; Range vwapLv{}; double vwapP1=0, vwapP2=0; bool vwapByLevels=true; // VWAP
  Range imbLv{}; double imbP1=0, imbP2=0; bool imbByLevels=true;                       // Imbalance
  std::string metaField;                                   // Meta
};

// Parse / check
std::optional<ObKey> parseObKey(const std::string& keyStr);
bool                 isObKey(const std::string& keyStr);
std::string          formatObKey(const ObKey& k);

// Helpers
inline const char* toString(Mode m){ return m==Mode::Per?"per":"agg"; }
inline const char* toString(Side s){ return s==Side::Bid?"bid":"ask"; }

} // namespace gma::ob
