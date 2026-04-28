#pragma once

#include <string>
#include <vector>

namespace gma {

/// Configurable mapping from source-specific field names to the canonical
/// fields that Dispatcher needs for TA computation (price, volume).
///
/// Each vector is tried in order; the first match in the JSON payload wins.
/// Defaults match the legacy hardcoded behaviour (NASDAQ-style feeds).
struct SourceProfile {
    std::string name = "default";

    /// Field names to try when extracting the trade price from a tick payload.
    std::vector<std::string> priceFields  = {"lastPrice", "price", "last", "px"};

    /// Field names to try when extracting the volume from a tick payload.
    std::vector<std::string> volumeFields = {"volume", "vol", "qty", "size"};

    /// Field names to try for best bid price (empty = not extracted).
    std::vector<std::string> bidFields;

    /// Field names to try for best ask price (empty = not extracted).
    std::vector<std::string> askFields;

    /// Field name for event timestamp in nanoseconds (empty = not extracted).
    std::string timestampField;

    /// Whether the full TA indicator suite runs on every tick.
    /// When false, only base metrics (lastPrice, openPrice, highPrice, lowPrice,
    /// volume, bid, ask, spread, timestamp) are stored — no SMA/EMA/RSI/etc.
    bool taEnabled = true;
};

} // namespace gma
