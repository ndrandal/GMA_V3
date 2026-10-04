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
   │  dispatchEvent()  │  ← routes events to the engine `Dispatcher` + `OrderBookManager`
   └──────────────────┘
```

> **Paths moved (ENC-31/ENC-35).** Feed code is **connector-owned**: it lives
> under `connectors/market/{include,src}/gma/...`, not at engine level. There is
> **no `MarketDispatcher`** — `WsFeedClient::dispatchEvent` writes order-book
> events into `OrderBookManager` and tick events into the engine `Dispatcher`
> (`Dispatcher::onTick`), which runs the `MarketTickComputer`.

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

Defined in `connectors/market/include/gma/feed/FeedEvent.hpp`.

## Field mapping (`MarketFieldMap`)

`gma::market::MarketFieldMap` (in
`connectors/market/include/gma/market/MarketFieldMap.hpp`) configures how the
`MarketTickComputer` extracts price/volume/bid/ask/timestamp from tick payloads.
Each field list is tried in order; the first match wins.

```cpp
struct MarketFieldMap {
    std::string name = "default";
    std::vector<std::string> priceFields  = {"lastPrice", "price", "last", "px"};
    std::vector<std::string> volumeFields = {"volume", "vol", "qty", "size"};
    std::vector<std::string> bidFields  = {"bid"};   // ENC-1028: were empty
    std::vector<std::string> askFields  = {"ask"};   // ENC-1028: were empty
    std::string timestampField;                // empty = not extracted
    bool taEnabled = true;                     // false = skip SMA/EMA/RSI/etc.
};
```

> **ENC-35:** this replaced the old engine-level `gma::SourceProfile` /
> `Config::sourceProfile` member. The engine no longer knows about
> market-flavored fields — the market connector owns the struct and populates it
> from the `market.source.*` config namespace through `ConfigNamespaceRegistry`.

> **ENC-1028 — what this layer can and cannot do for you.** `bidFields` and
> `askFields` defaulted **empty**, so the bid/ask scan in
> `MarketTickComputer::compute` ran **zero times** for every source that had not
> configured them by hand. They now default to the canonical names, for the same
> reason `priceFields` leads with `lastPrice`.
>
> But fixing the default does **not** give an L2/L3 source a bid or an ask, and
> it is worth being clear about why: **ITCH carries no top-of-book field.** Its
> message types are `add_order`, `add_order_mpid`, `order_executed`,
> `order_cancel`, `order_delete`, `order_replace` and `trade`, and not one of
> them has a bid or ask member — verified against the live feed and pinned by
> `tests/feed/ItchAtomicsTest.cpp::NoItchMessageTypeCarriesABidOrAnAsk`. There
> is nothing to alias. For such a source the top of book is injected onto the
> tick from the reconstructed book by `WsFeedClient::dispatchEvent`, and this
> field map resolves it from there.
>
> **So if you are writing an adapter for a depth protocol, do not reach for
> `bidFields`.** Emit the book mutations and let the ingress sample the book.
> `bidFields` is for a source that hands you a pre-aggregated quote.

### Timestamps: convert the basis in the adapter (ENC-1028)

`TickEvent::timestampNs` and `gma::Event::timestampNs` are **nanoseconds since
the Unix epoch**, and `0` means *the source did not report one*. The unit is
part of the contract: an adapter whose protocol uses a different basis must
convert **before** the value reaches these fields.

`ItchAdapter` is exactly that case and is the worked example. ITCH 5.0 stamps
nanoseconds since **UTC midnight** and puts no date on the wire; the two bases
differ by ~1.7e18 ns, so copying one into the other produces a timestamp that
reads as a believable *time of day* in 1970 — wrong by 54 years, and wrong in a
way nothing downstream can detect. `ItchAdapter::itchTimestampToEpochNs` rebases
it (the feed supplies the time of day, GMA supplies the UTC day, including the
day-boundary case) and returns `0` for every input it cannot resolve rather than
inventing a time.

Prefer the typed field to a payload member, for two reasons:

1. **Precision.** `Dispatcher::onTick` raw-injects every *numeric* payload
   member into the `AtomicStore` as a `double` (ENC-1007). Epoch nanos exceed
   2^53, so a payload-borne timestamp is silently rounded to ~256 ns.
   `MarketTickComputer` stores the `timestamp` atomic as a **string** for the
   same reason.
2. **Ambiguity.** A payload key named `timestamp` is what the raw ITCH wire
   already calls its since-midnight value, so the same name would mean two
   different bases depending on which side of the adapter you read it from.
   `MarketFieldMap::timestampField` is therefore deliberately **still empty by
   default** — a payload-borne timestamp has to be opted into by a deployment
   that knows its source's basis.

### Example: Coinbase

A Coinbase feed sends `"price"` and `"last_size"` instead of `"lastPrice"` and
`"volume"`. Configure it under the `market.source.*` namespace (the bare
`source.*` prefix still works as a one-release deprecation alias):

```ini
market.source.name = coinbase
market.source.priceFields = price
market.source.volumeFields = last_size,size
```

No code changes required — just config.

## Writing a New Adapter

Implement `IFeedAdapter::translate()`:

```cpp
// connectors/market/include/gma/feed/CoinbaseAdapter.hpp
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
// connectors/market/src/feed/CoinbaseAdapter.cpp
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

Wire it up **via the ingress factory + INI** (ENC-31) — *not* by editing
`main.cpp`. The `market.wsclient` factory in
`connectors/market/src/MarketConnector.cpp` constructs a `WsFeedClient` with the
adapter named by the `ingress.N.adapter` key, so adding a `coinbase` adapter
means teaching that factory the new adapter name and adding an INI entry:

```ini
ingress.0.kind    = market.wsclient
ingress.0.url     = wss://ws-feed.exchange.coinbase.com
ingress.0.adapter = coinbase
ingress.0.symbols = BTC-USD,ETH-USD
```

The composition root reads `cfg.ingress[]`, looks the `kind` up in
`IngressRegistry`, and drives the ingress source's lifecycle centrally. There is
no hand-wired `WsFeedClient` construction in `main.cpp`.

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
| `connectors/market/include/gma/feed/IFeedAdapter.hpp` | Adapter interface |
| `connectors/market/include/gma/feed/FeedEvent.hpp` | Canonical event types |
| `connectors/market/include/gma/feed/ItchAdapter.hpp` | ITCH adapter header |
| `connectors/market/src/feed/ItchAdapter.cpp` | ITCH adapter implementation |
| `connectors/market/include/gma/market/MarketFieldMap.hpp` | Configurable field mapping (replaces `SourceProfile`) |
| `connectors/market/include/gma/ws/WsFeedClient.hpp` | WebSocket transport (adapter-agnostic) |
| `connectors/market/src/ws/WsFeedClient.cpp` | Transport implementation + event dispatch |
| `tests/feed/ItchAdapterTest.cpp` | ITCH adapter unit tests |
| `tests/connectors/MarketFieldMapTest.cpp` | `MarketFieldMap` field-mapping tests |
| `tests/feed/FeedEventDispatchTest.cpp` | FeedEvent variant type tests |

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

### MarketFieldMapTest (`tests/connectors/MarketFieldMapTest.cpp`)

Verifies that the `MarketTickComputer` respects `MarketFieldMap` field mappings:

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
./gma_tests --gtest_filter="ItchAdapter*:MarketFieldMap*:FeedEvent*"

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
