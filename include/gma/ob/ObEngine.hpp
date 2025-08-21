#pragma once
#include "gma/ob/ObSnapshot.hpp"
#include "gma/ob/ObKey.hpp"

namespace gma::ob {

// Core entry
double eval(const Snapshot& s, const ObKey& k);

// Exposed helpers (unit-testable)
double bestPrice(const Snapshot&, Side);
double bestSize(const Snapshot&, Side);
double spread(const Snapshot&);
double mid(const Snapshot&);
double levelIdx(const Snapshot&, const LevelIdx&);
double levelPx (const Snapshot&, const LevelPx&, double tick);
double rangeIdxReduce(const Snapshot&, const RangeIdxSpec&);
double rangePxReduce (const Snapshot&, const RangePxSpec&, double tick);
double cumLevels(const Snapshot&, Side, int N, Target);
double vwapLevels(const Snapshot&, Side, Range);
double vwapPriceBand(const Snapshot&, Side, double p1, double p2, double tick);
double imbalanceLevels(const Snapshot&, Range);
double imbalanceBand  (const Snapshot&, double p1, double p2, double tick);
double meta(const Snapshot&, const std::string& field);

} // namespace gma::ob
