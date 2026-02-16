// File: include/gma/AtomicFunctions.hpp
#pragma once

#include <string>
#include <deque>
#include "gma/SymbolHistory.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/util/Config.hpp"

namespace gma {

/**
 * Compute a suite of atomic values (statistics/indicators) for the given symbol
 * based on its price/volume history, storing results into AtomicStore.
 *
 * TA periods are read from cfg. Keys written depend on configured periods,
 * e.g. "sma_5", "sma_20" for cfg.taSMA={5,20}.
 */
void computeAllAtomicValues(
    const std::string& symbol,
    const SymbolHistory& hist,
    AtomicStore& store,
    const util::Config& cfg = util::Config{}
);

} // namespace gma
