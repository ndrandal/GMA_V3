# Writing Feed Adapters

This guide covers everything needed to integrate a new data source into GMA through the feed adapter interface. By the end you'll have a working adapter, a matching `SourceProfile`, and a test suite.

## Overview

An adapter is a single class that implements `IFeedAdapter`. It receives raw wire messages (strings) and returns canonical `FeedEvent` objects. GMA handles everything else — transport, reconnection, order book management, TA computation, and client notification.

```
Your data source  ──raw messages──▸  YourAdapter::translate()  ──FeedEvents──▸  GMA engine
```

No changes to the core engine are required. No recompilation of existing code. Just a new class, a config tweak, and a one-line wiring change in `main.cpp`.

## Step 1: Understand your source's wire format

Before writing code, map your source's message types to GMA's canonical events:

| Your source sends... | GMA event to emit |
|----------------------|-------------------|
| Trade / match / execution | `ObTradeEvent` + `TickEvent` |
| New order / order opened | `ObAddEvent` |
| Order size/price changed | `ObUpdateEvent` |
| Order cancelled / removed | `ObDeleteEvent` |
| Market data snapshot (price, volume) | `TickEvent` |
| Symbol metadata (tick size) | `ObTickSizeEvent` |
| Session reset / reconnect | `ObResetEvent` |

Not every source will have all of these. A price-only feed (no order book) only needs to emit `TickEvent`. An L3 order book feed will emit all of them.

## Step 2: Implement the adapter

### Header

```cpp
// include/gma/feed/BinanceAdapter.hpp
#pragma once
#include "gma/feed/IFeedAdapter.hpp"
#include <unordered_map>
#include <string>

namespace gma::feed {

class BinanceAdapter : public IFeedAdapter {
public:
    std::vector<FeedEvent> translate(const std::string& rawMessage) override;

private:
    // Put any stateful protocol tracking here.
    // Examples:
    //   - Symbol normalization tables
    //   - Previous-update-id tracking for gap detection
    //   - Snapshot state machines
    //   - Partial message buffers
};

} // namespace gma::feed
```

### Implementation

```cpp
// src/feed/BinanceAdapter.cpp
#include "gma/feed/BinanceAdapter.hpp"

#include <rapidjson/document.h>

namespace gma::feed {

std::vector<FeedEvent> BinanceAdapter::translate(const std::string& rawMessage) {
    // 1. Parse
    rapidjson::Document doc;
    doc.Parse(rawMessage.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return {};

    std::vector<FeedEvent> out;

    // 2. Route by message type
    //    Binance uses "e" as the event type field
    if (!doc.HasMember("e") || !doc["e"].IsString()) return {};
    const std::string type = doc["e"].GetString();

    if (type == "trade") {
        handleTrade(doc, out);
    } else if (type == "depthUpdate") {
        handleDepthUpdate(doc, out);
    }
    // ... more message types

    return out;
}

} // namespace gma::feed
```

### Rules for `translate()`

1. **Always return an empty vector for messages you can't parse.** Never throw — the caller logs exceptions but you don't want to kill the read loop.

2. **One call can return multiple events.** An ITCH `order_executed` produces an `ObDeleteEvent` (or `ObUpdateEvent`), an `ObTradeEvent`, and a `TickEvent` — all from a single wire message. Return them all in one vector.

3. **Event order matters.** `WsFeedClient::dispatchEvent` processes them sequentially. Emit OB mutations before the trade tick so the book is updated before TA fires.

4. **The adapter owns all protocol state.** Anything stateful about the source protocol (order tracking, sequence numbers, symbol mappings) lives in the adapter, not in WsFeedClient.

## Step 3: Emit the right events

### TickEvent — trigger TA computation

This is the most important event. If your adapter doesn't emit `TickEvent`, no TA indicators will compute.

```cpp
TickEvent makeTickEvent(const std::string& symbol, double price, double volume) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& a = payload->GetAllocator();
    payload->AddMember("lastPrice", price, a);
    payload->AddMember("volume", volume, a);

    return TickEvent{symbol, std::move(payload)};
}
```

The `TickEvent::payload` is a RapidJSON Document. MarketDispatcher extracts price and volume from it using the `SourceProfile` field aliases. The payload can contain any additional numeric fields — clients can subscribe to them by name.

**Key point:** The field names in the payload must match either the `SourceProfile` aliases (for TA to fire) or the field names that clients subscribe to (for raw-field listeners). If your source calls the price field `"last_trade_price"`, you have two choices:

- **Option A:** Normalize it in the adapter — `AddMember("lastPrice", ...)` — and use the default `SourceProfile`
- **Option B:** Keep the source's field name — `AddMember("last_trade_price", ...)` — and configure `SourceProfile.priceFields = {"last_trade_price"}`

Option A is simpler when you control the adapter. Option B is better when you want the adapter to be a transparent pass-through.

### ObAddEvent — add an order to the book

```cpp
out.push_back(ObAddEvent{
    .symbol   = "AAPL",
    .orderId  = 12345,         // unique order ID from the source
    .side     = Side::Bid,     // Side::Bid or Side::Ask
    .price    = 187.42,        // double — converted to ticks internally by OrderBookManager
    .size     = 100,           // uint64_t shares/contracts
    .priority = 0              // optional: time priority within a price level
});
```

### ObUpdateEvent — modify an existing order

```cpp
out.push_back(ObUpdateEvent{
    .symbol   = "AAPL",
    .orderId  = 12345,
    .newPrice = 187.50,          // std::optional — omit if unchanged
    .newSize  = std::nullopt     // std::optional — omit if unchanged
});
```

### ObDeleteEvent — remove an order

```cpp
out.push_back(ObDeleteEvent{
    .symbol  = "AAPL",
    .orderId = 12345
});
```

### ObTradeEvent — record a trade

```cpp
out.push_back(ObTradeEvent{
    .symbol    = "AAPL",
    .price     = 187.42,
    .size      = 100,
    .aggressor = Aggressor::Buy   // Buy, Sell, or Unknown
});
```

Trades are reported to `OrderBookManager` for book analytics. If you also want TA indicators to fire on this trade, emit a `TickEvent` alongside it.

### ObTickSizeEvent — set minimum price increment

```cpp
out.push_back(ObTickSizeEvent{
    .symbol   = "AAPL",
    .tickSize = 0.01     // $0.01 for US equities
});
```

OrderBookManager stores prices as integer ticks internally. The tick size must be set before any order events for that symbol.

### ObResetEvent — session reset

```cpp
out.push_back(ObResetEvent{
    .symbol = "AAPL",
    .epoch  = 42          // monotonically increasing epoch counter
});
```

Triggers OrderBookManager to clear stale state for the symbol and request a fresh snapshot.

## Step 4: Handle stateful protocols

Many feeds require tracking state across messages. Common patterns:

### Order state tracking (L3 feeds)

ITCH, OUCH, and similar L3 feeds report partial fills as deltas ("50 shares executed on order 12345"). You need to track each order's remaining size to know whether a fill is partial (emit `ObUpdateEvent`) or complete (emit `ObDeleteEvent`).

See `ItchAdapter` for the reference implementation — it maintains an `orders_` map of `{orderRef → OrderState}`.

### Symbol mapping

Some feeds use integer IDs instead of ticker strings. The adapter maintains its own lookup table.

```cpp
// In your adapter's private state:
std::unordered_map<int, std::string> idToSymbol_;

// When you receive a symbol definition message:
idToSymbol_[msg.symbolId] = msg.ticker;

// When you receive a trade/order message:
std::string symbol = idToSymbol_[msg.symbolId];
```

### Sequence gap detection

Many feeds include sequence numbers. Track the last-seen sequence and emit `ObResetEvent` when you detect a gap:

```cpp
if (seq != lastSeq_ + 1) {
    out.push_back(ObResetEvent{symbol, ++epoch_});
}
lastSeq_ = seq;
```

### L2 depth feeds (no order IDs)

Some sources (Coinbase L2, Binance depth) send aggregated price levels, not individual orders. Generate synthetic order IDs from the price level:

```cpp
// Use price-level hash as a stable order ID
uint64_t syntheticId = std::hash<double>{}(price) ^ std::hash<std::string>{}(symbol);
```

Or use `OrderBookManager::onLevelSummary()` via an `ObAddEvent` with a well-known ID scheme. The key constraint is that `orderId` must be consistent across updates — the same price level must always use the same ID.

## Step 5: Configure the SourceProfile

If your adapter normalizes field names in the `TickEvent` payload to match the defaults (`lastPrice`, `volume`), you're done — the default `SourceProfile` works.

If your adapter passes through the source's native field names, configure the profile in the config file:

```ini
# For a crypto feed that sends "p" for price and "q" for quantity
source.name = binance
source.priceFields = p,price,lastPrice
source.volumeFields = q,volume,qty
```

Or set it programmatically before constructing `MarketDispatcher`:

```cpp
cfg.sourceProfile.name = "binance";
cfg.sourceProfile.priceFields = {"p", "price", "lastPrice"};
cfg.sourceProfile.volumeFields = {"q", "volume", "qty"};
```

The field lists are tried in order. Put your source's primary field name first, then add fallbacks for flexibility.

## Step 6: Wire it into main.cpp

```cpp
#include "gma/feed/BinanceAdapter.hpp"

// In the feed client setup section:
auto adapter = std::make_unique<gma::feed::BinanceAdapter>();
feedClient = std::make_shared<gma::ws::WsFeedClient>(
    ioc, dispatcher.get(), obManager.get(),
    feedUrl, std::move(adapter), cfg.feedSymbols);
feedClient->start();
```

The adapter is moved into `WsFeedClient` via `unique_ptr` — the client owns its lifetime.

## Step 7: Write tests

Create `tests/feed/BinanceAdapterTest.cpp`. The pattern is straightforward:

```cpp
#include "gma/feed/BinanceAdapter.hpp"
#include <gtest/gtest.h>

using namespace gma::feed;

TEST(BinanceAdapterTest, TradeMessageEmitsTradeAndTick) {
    BinanceAdapter adapter;

    std::string msg = R"({
        "e": "trade",
        "s": "BTCUSDT",
        "p": "42000.50",
        "q": "0.001",
        "m": true
    })";

    auto events = adapter.translate(msg);

    // Verify correct events emitted
    ASSERT_FALSE(events.empty());
    // ... assert on specific event types and field values
}
```

### What to test

| Category | What to verify |
|----------|---------------|
| **Happy path** | Each message type produces the correct event(s) with correct field values |
| **Field parsing** | Strings vs numbers, precision, edge values (0, negative, very large) |
| **Side/aggressor** | Correct mapping from source convention to `Side::Bid/Ask`, `Aggressor::Buy/Sell` |
| **Missing fields** | Incomplete messages return empty vector, never crash |
| **Invalid input** | Malformed JSON, empty string, wrong types — all return empty |
| **Stateful sequences** | Multi-message workflows: add → partial fill → cancel → verify state cleanup |
| **Symbol normalization** | Padding trimmed, case handled, mapping tables work |
| **Event ordering** | OB mutation events come before the trade tick in the returned vector |

Copy the helper templates from `tests/feed/ItchAdapterTest.cpp` — `hasEvent<T>`, `getEvent<T>`, `countEvents<T>` — they work with any adapter.

### Running your tests

```bash
# Reconfigure cmake to pick up the new test file
cd build && cmake .. -DGMA_BUILD_TESTS=ON

# Run just your tests
./gma_tests --gtest_filter="BinanceAdapterTest.*"
```

## Adapter checklist

Before shipping a new adapter:

- [ ] `translate()` handles every message type the source sends
- [ ] Invalid/malformed messages return empty vector (never throw)
- [ ] Trades emit both `ObTradeEvent` and `TickEvent` (so both the book and TA update)
- [ ] `ObTickSizeEvent` is emitted before any order events for a symbol
- [ ] Stateful protocol logic (order tracking, sequence gaps) is tested with multi-message sequences
- [ ] `SourceProfile` is configured if field names differ from defaults
- [ ] Tests exist for happy path, error cases, and stateful lifecycle
- [ ] The adapter compiles and all tests pass: `ctest --output-on-failure`

## Reference: IFeedAdapter interface

```cpp
// include/gma/feed/IFeedAdapter.hpp
namespace gma::feed {

class IFeedAdapter {
public:
    virtual ~IFeedAdapter() = default;
    virtual std::vector<FeedEvent> translate(const std::string& rawMessage) = 0;
    virtual std::string subscribeMessage(const std::vector<std::string>& symbols);
    // Default: {"action":"subscribe","symbols":[...]}
    // Override to match your source's subscription protocol.
};

}
```

## Reference: FeedEvent types

```cpp
// include/gma/feed/FeedEvent.hpp
namespace gma::feed {

struct TickEvent        { string symbol; shared_ptr<rapidjson::Document> payload; uint64_t timestampNs=0; };
struct ObAddEvent       { string symbol; uint64_t orderId; Side side; double price; uint64_t size; uint64_t priority=0; };
struct ObUpdateEvent    { string symbol; uint64_t orderId; optional<double> newPrice; optional<uint64_t> newSize; };
struct ObDeleteEvent    { string symbol; uint64_t orderId; };
struct ObTradeEvent     { string symbol; double price; uint64_t size; Aggressor aggressor=Unknown; uint64_t timestampNs=0; };
struct ObTickSizeEvent  { string symbol; double tickSize; };
struct ObResetEvent     { string symbol; uint32_t epoch=0; };

using FeedEvent = variant<TickEvent, ObAddEvent, ObUpdateEvent, ObDeleteEvent,
                          ObTradeEvent, ObTickSizeEvent, ObResetEvent>;
}
```

## Reference: Existing adapters

| Adapter | Source | Stateful | File |
|---------|--------|----------|------|
| `ItchAdapter` | NASDAQ ITCH JSON | Yes (order tracking, symbol mapping) | `src/feed/ItchAdapter.cpp` |
