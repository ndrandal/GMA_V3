// File: include/gma/AtomicFunctions.hpp
#pragma once

#include <string>
#include <deque>
#include <vector>
#include "gma/SymbolHistory.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/util/Config.hpp"

namespace gma {

/**
 * Compute a suite of atomic values (statistics/indicators) for the given symbol
 * based on its price/volume history, storing results into AtomicStore.
 *
 * Accepts a contiguous vector for optimal cache locality during TA computation.
 * TA periods are read from cfg. Keys written depend on configured periods,
 * e.g. "sma_5", "sma_20" for cfg.taSMA={5,20}.
 */
void computeAllAtomicValues(
    const std::string& symbol,
    const std::vector<TickEntry>& hist,
    AtomicStore& store,
    const util::Config& cfg = util::Config{}
);

/**
 * Register basic per-field statistical functions into FunctionMap.
 * These operate on vector<double> (per-field history) and are evaluated
 * by MarketDispatcher::computeAndStoreAtomics() on every tick.
 *
 * Registered functions: mean, sum, min, max, last, first, count, stddev
 */
void registerBuiltinFunctions();

} // namespace gma
