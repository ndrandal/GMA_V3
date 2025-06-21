// File: include/gma/AtomicFunctions.hpp
#pragma once

#include <string>
#include <deque>
#include "gma/SymbolHistory.hpp"    // defines SymbolHistory (std::deque<TickEntry>)
#include "gma/AtomicStore.hpp"       // forward-declares AtomicStore and set methods

namespace gma {

/**
 * Compute a suite of atomic values (statistics/indicators) for the given symbol
 * based on its price/volume history, storing results into AtomicStore.
 *
 * Keys written include:
 *   "lastPrice", "openPrice", "highPrice", "lowPrice",
 *   "mean", "median",
 *   plus additional technicals when history size > 1 (prevClose, vwap, sma_5, sma_20,
 *   ema_12, ema_26, rsi_14, macd_line, macd_signal, bollinger bands,
 *   momentum_10, roc_10, atr_14, volume, volume_avg_20, obv,
 *   volatility_rank, isHalted, marketState, timeSinceOpen, timeUntilClose).
 */
void computeAllAtomicValues(
    const std::string& symbol,
    const SymbolHistory& hist,
    AtomicStore& store
);

} // namespace gma
