#pragma once

#include <string>
#include <vector>

namespace gma::market {

// Connector-side mapping from source-specific JSON field names to the
// canonical fields the market tick computer needs (price, volume, bid,
// ask, timestamp). Each vector is tried in order; first match wins.
// Defaults match the legacy NASDAQ-style feed payload.
//
// Replaces the engine-level gma::SourceProfile per ENC-35 (audit V6):
// engine code no longer knows about market-flavored fields. The connector
// owns this struct and populates it from the `market.source.*` config
// namespace via ConfigNamespaceRegistry.
struct MarketFieldMap {
  std::string name = "default";

  // Field names to try when extracting the trade price from a tick payload.
  std::vector<std::string> priceFields  = {"lastPrice", "price", "last", "px"};

  // Field names to try when extracting the volume from a tick payload.
  std::vector<std::string> volumeFields = {"volume", "vol", "qty", "size"};

  // Field names to try for best bid price (empty = not extracted).
  std::vector<std::string> bidFields;

  // Field names to try for best ask price (empty = not extracted).
  std::vector<std::string> askFields;

  // Field name for event timestamp in nanoseconds (empty = not extracted).
  std::string timestampField;

  // Whether the full TA indicator suite runs on every tick.
  // When false, only base metrics (lastPrice, openPrice, highPrice, lowPrice,
  // volume, bid, ask, spread, timestamp) are stored — no SMA/EMA/RSI/etc.
  bool taEnabled = true;
};

} // namespace gma::market
