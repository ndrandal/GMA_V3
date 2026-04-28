#pragma once
#include <vector>
#include <optional>
#include <string>
#include "gma/book/OrderBook.hpp"  // for Price, Side

namespace gma {

// One level’s new total after a mutation
struct LevelDelta {
    Side side;
    Price price;
    uint64_t totalSize = 0;
};

// Streamed incremental change
struct BookDelta {
    std::string symbol;
    uint64_t seq = 0;  // manager-emitted monotonically increasing sequence (per symbol)
    std::vector<LevelDelta> levels;  // only levels whose totals changed
    std::optional<std::pair<Price,uint64_t>> bid; // present iff TOB bid changed
    std::optional<std::pair<Price,uint64_t>> ask; // present iff TOB ask changed
};

// Top-N snapshot at a moment in time
struct DepthSnapshot {
    std::string symbol;
    uint64_t seq = 0;     // same counter as deltas
    uint32_t epoch = 0;   // from FeedState
    std::vector<std::pair<Price,uint64_t>> bids; // ordered best->worse
    std::vector<std::pair<Price,uint64_t>> asks; // ordered best->worse
};

} // namespace gma
