# Feed Adapters

GMA's feed ingestion is source-agnostic. External data sources (NASDAQ ITCH, Coinbase, FIX, custom JSON, etc.) are supported through pluggable **feed adapters** that translate vendor-specific wire formats into canonical `FeedEvent` types.

## Architecture

```
External Feed (WebSocket/TCP)
        │
        ▼
   ┌─────────────┐
   │ WsFeedClient │  ← transport only (connect, reconnect, TLS, read loop)
   └──────┬──────┘
          │ raw message string
          ▼
   ┌─────────────┐
   │ IFeedAdapter │  ← protocol translation (ITCH, Coinbase, etc.)
   │  translate() │
   └──────┬──────┘
          │ vector<FeedEvent>
          ▼
   ┌──────────────────┐
   │  dispatchEvent()  │  ← routes events to MarketDispatcher + OrderBookManager
   └──────────────────┘
```

`WsFeedClient` handles connection management. The adapter handles protocol logic. Neither knows about the other's internals.

## FeedEvent Types

Every adapter produces a sequence of these canonical events per raw message:

| Event | Purpose | Key fields |
|-------|---------|------------|
| `TickEvent` | Market data tick for TA computation | `symbol`, `payload` (JSON DOM) |
| `ObAddEvent` | Add an order to the book | `symbol`, `orderId`, `side`, `price`, `size` |
| `ObUpdateEvent` | Update an existing order | `symbol`, `orderId`, `newPrice?`, `newSize?` |
| `ObDeleteEvent` | Remove an order from the book | `symbol`, `orderId` |
| `ObTradeEvent` | A trade executed on the venue | `symbol`, `price`, `size`, `aggressor` |
| `ObTickSizeEvent` | Set tick size for a symbol | `symbol`, `tickSize` |
| `ObResetEvent` | Reset/new epoch for a symbol's book | `symbol`, `epoch` |

Defined in `include/gma/feed/FeedEvent.hpp`.

## Source Profiles

`SourceProfile` (in `include/gma/SourceProfile.hpp`) configures how `MarketDispatcher` extracts price and volume from tick payloads. Each field list is tried in order; the first match wins.

```cpp
struct SourceProfile {
    std::string name = "default";
    std::vector<std::string> priceFields  = {"lastPrice", "price", "last", "px"};
    std::vector<std::string> volumeFields = {"volume", "vol", "qty", "size"};
    std::vector<std::string> bidFields;        // empty = not extracted
    std::vector<std::string> askFields;        // empty = not extracted
    std::string timestampField;                // empty = not extracted
    bool taEnabled = true;                     // false = skip SMA/EMA/RSI/etc.
};
```

Set via the `sourceProfile` member in `Config`. The defaults match NASDAQ-style feeds and are backward compatible.

### Example: Coinbase

A Coinbase feed sends `"price"` and `"last_size"` instead of `"lastPrice"` and `"volume"`:

```ini
source.name = coinbase
source.priceFields = price
source.volumeFields = last_size,size
```

No code changes required — just config.

## Writing a New Adapter

Implement `IFeedAdapter::translate()`:

```cpp
// include/gma/feed/CoinbaseAdapter.hpp
#pragma once
#include "gma/feed/IFeedAdapter.hpp"

namespace gma::feed {

class CoinbaseAdapter : public IFeedAdapter {
public:
    std::vector<FeedEvent> translate(const std::string& rawMessage) override;
private:
    // Any stateful protocol tracking goes here
};

} // namespace gma::feed
```

```cpp
// src/feed/CoinbaseAdapter.cpp
#include "gma/feed/CoinbaseAdapter.hpp"
#include <rapidjson/document.h>

namespace gma::feed {

std::vector<FeedEvent> CoinbaseAdapter::translate(const std::string& rawMessage) {
    rapidjson::Document doc;
    doc.Parse(rawMessage.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return {};

    std::vector<FeedEvent> out;
    const std::string type = doc["type"].GetString();

    if (type == "match") {
        // Trade — emit OB trade + tick for TA
        std::string symbol = doc["product_id"].GetString();
        double price = std::stod(doc["price"].GetString());
        uint64_t size = static_cast<uint64_t>(std::stod(doc["size"].GetString()));

        out.push_back(ObTradeEvent{symbol, price, size, Aggressor::Unknown});

        // Build a tick so TA indicators fire
        auto payload = std::make_shared<rapidjson::Document>();
        payload->SetObject();
        auto& a = payload->GetAllocator();
        payload->AddMember("price", price, a);
        payload->AddMember("last_size", static_cast<double>(size), a);
        out.push_back(TickEvent{symbol, std::move(payload)});
    }
    // Handle "open", "done", "change" for OB events...

    return out;
}

} // namespace gma::feed
```

Wire it up in `main.cpp`:

```cpp
auto adapter = std::make_unique<gma::feed::CoinbaseAdapter>();
feedClient = std::make_shared<gma::ws::WsFeedClient>(
    ioc, dispatcher.get(), obManager.get(),
    feedUrl, std::move(adapter), cfg.feedSymbols);
```

## Existing Adapters

### ItchAdapter

Translates NASDAQ ITCH JSON protocol messages. Handles:

- `stock_directory` — symbol registration + $0.01 tick size
- `add_order` / `add_order_mpid` — order book additions
- `order_executed` — partial/full fills (emits OB update/delete + trade + tick)
- `order_cancel` — partial cancellations
- `order_delete` — full removals
- `order_replace` — delete old + add new
- `trade` — direct trade reports
- `system_event` — logged, no events emitted

Maintains internal state for `stockLocate → symbol` mapping and live order tracking (needed to resolve partial fills into absolute sizes).

## TCP FeedServer

The TCP `FeedServer` (port 9001) accepts the GMA wire protocol directly and is **not** behind an adapter. It supports:

- Market ticks: `{"symbol":"AAPL","lastPrice":187.42,"volume":350}`
- OB commands: `{"type":"ob","action":"add","symbol":"AAPL","id":1,"side":"bid","price":187.40,"size":100}`
- Control: `{"type":"control","action":"reset","symbol":"AAPL"}`

This is GMA's own protocol — it's already source-agnostic. External vendor feeds go through `WsFeedClient` + an adapter.

## File Reference

| File | Purpose |
|------|---------|
| `include/gma/feed/IFeedAdapter.hpp` | Adapter interface |
| `include/gma/feed/FeedEvent.hpp` | Canonical event types |
| `include/gma/feed/ItchAdapter.hpp` | ITCH adapter header |
| `src/feed/ItchAdapter.cpp` | ITCH adapter implementation |
| `include/gma/SourceProfile.hpp` | Configurable field mapping |
| `include/gma/ws/WsFeedClient.hpp` | WebSocket transport (adapter-agnostic) |
| `src/ws/WsFeedClient.cpp` | Transport implementation + event dispatch |
| `tests/feed/ItchAdapterTest.cpp` | ITCH adapter unit tests (21 tests) |
| `tests/feed/SourceProfileTest.cpp` | SourceProfile + MarketDispatcher field mapping tests (10 tests) |
| `tests/feed/FeedEventDispatchTest.cpp` | FeedEvent variant type tests (13 tests) |

## Testing

Tests live in `tests/feed/` and cover three areas:

### ItchAdapterTest (21 tests)

Exercises every ITCH message type through `ItchAdapter::translate()`:

- **stock_directory** — symbol registration, trailing-space trimming, $0.01 tick size emission
- **add_order / add_order_mpid** — ObAddEvent production, side parsing (B/S), missing field rejection
- **order_executed** — full fill (delete + trade + tick), partial fill (update + trade + tick), unknown order handling
- **order_cancel** — partial cancel (size reduction), full cancel (delete)
- **order_delete** — removal, double-delete produces nothing
- **order_replace** — delete old + add new, preserves side, unknown orig produces nothing
- **trade** — ObTradeEvent + TickEvent, aggressor parsing, string price format ("185.2500")
- **system_event** — logs only, no events emitted
- **Invalid input** — malformed JSON, missing type field, unknown type
- **Full lifecycle** — stateful test: directory → add → partial fill → partial cancel → replace → full fill → verify cleanup

### SourceProfileTest (10 tests)

Verifies that MarketDispatcher respects `SourceProfile` field mappings:

- **Default profile** — `lastPrice` triggers TA, `price` fallback works
- **Custom profile** — non-standard field names (e.g., `last_trade_price`, `last_trade_volume`) trigger TA correctly
- **Unmapped fields** — tick with `lastPrice` is ignored when profile only maps `trade_px`
- **Priority order** — first matching field in the list wins when tick contains multiple candidates
- **Raw listeners** — listeners on raw field names still fire regardless of profile config
- **No match** — no matching price field skips TA entirely
- **Dynamic skip list** — config-driven TA periods (e.g., `sma_7`, `sma_14`) are computed and stored
- **History accumulation** — multiple ticks build correct SMA with custom fields

### FeedEventDispatchTest (13 tests)

Validates the `FeedEvent` variant types:

- Construction and field access for all 7 event types
- Default values (priority=0, aggressor=Unknown, epoch=0)
- Optional fields on `ObUpdateEvent` (price-only, size-only, both)
- `std::visit` covers all variant alternatives
- Move semantics (payload transferred, not copied)
- Variant index stability (for future binary serialization)

### Running the tests

```bash
# All tests
cd build && ctest --output-on-failure

# Just feed adapter tests
./gma_tests --gtest_filter="ItchAdapter*:SourceProfile*:FeedEvent*"

# Just one suite
./gma_tests --gtest_filter="ItchAdapterTest.*"
```

### Writing tests for a new adapter

Follow the same pattern as `ItchAdapterTest.cpp`:

1. Construct your adapter: `YourAdapter adapter;`
2. Build raw JSON strings matching your source's wire format
3. Call `adapter.translate(msg)` and assert on the returned `vector<FeedEvent>`
4. Use the `hasEvent<T>`, `getEvent<T>`, `countEvents<T>` helpers from the test file
5. Test stateful sequences (order lifecycle) with multiple `translate()` calls on the same adapter instance
6. Test invalid/malformed input produces empty results
