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
  //
  // ENC-1028: these defaulted EMPTY, which made the bid/ask scan in
  // MarketTickComputer::compute run zero times for every source that did not
  // explicitly configure `market.source.bidFields` — i.e. all of them. The
  // canonical name goes first here for the same reason `lastPrice` leads
  // priceFields: an upstream that has already normalised to GMA's vocabulary
  // (the ITCH path now injects a book-derived `bid`/`ask` onto the tick —
  // see WsFeedClient::dispatchEvent) must resolve without per-deployment
  // config. Vendor aliases still go after it.
  std::vector<std::string> bidFields  = {"bid"};

  // Field names to try for best ask price (empty = not extracted).
  std::vector<std::string> askFields  = {"ask"};

  // Field name for event timestamp in NANOSECONDS SINCE THE UNIX EPOCH
  // (empty = not extracted).
  //
  // This is the FALLBACK path, for a pre-aggregated source that carries its
  // time inside the tick payload. The primary path is the typed
  // Event::timestampNs, which MarketTickComputer prefers when it is non-zero;
  // see the note there and on ItchAdapter::itchTimestampToEpochNs.
  //
  // Deliberately still EMPTY by default, and deliberately NOT defaulted to
  // "timestamp". A field literally named `timestamp` is exactly what raw ITCH
  // JSON calls its nanos-since-UTC-midnight value, so defaulting to that name
  // would scoop up a value on a DIFFERENT BASIS and store it as though it were
  // epoch nanos — a timestamp wrong by 54 years that still renders as a
  // believable time of day. A payload-borne timestamp has to be opted into by
  // a deployment that knows its source's basis.
  std::string timestampField;

  // Whether the full TA indicator suite runs on every tick.
  // When false, only base metrics (lastPrice, openPrice, highPrice, lowPrice,
  // volume, bid, ask, spread, timestamp) are stored — no SMA/EMA/RSI/etc.
  bool taEnabled = true;
};

} // namespace gma::market
