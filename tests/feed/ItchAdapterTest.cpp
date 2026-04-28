#include "gma/feed/ItchAdapter.hpp"
#include "gma/feed/FeedEvent.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <string>
#include <vector>

using namespace gma;
using namespace gma::feed;

// ---------------------------------------------------------------------------
// Helper — build a JSON string from key/value pairs
// ---------------------------------------------------------------------------
namespace {

class JsonBuilder {
public:
    JsonBuilder() { w_.StartObject(); }

    JsonBuilder& str(const char* key, const char* val) {
        w_.Key(key); w_.String(val); return *this;
    }
    JsonBuilder& num(const char* key, double val) {
        w_.Key(key); w_.Double(val); return *this;
    }
    JsonBuilder& uint(const char* key, uint64_t val) {
        w_.Key(key); w_.Uint64(val); return *this;
    }
    JsonBuilder& integer(const char* key, int val) {
        w_.Key(key); w_.Int(val); return *this;
    }

    std::string build() {
        w_.EndObject();
        return sb_.GetString();
    }

private:
    rapidjson::StringBuffer sb_;
    rapidjson::Writer<rapidjson::StringBuffer> w_{sb_};
};

template <typename T>
bool hasEvent(const std::vector<FeedEvent>& events) {
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) return true;
    }
    return false;
}

template <typename T>
const T& getEvent(const std::vector<FeedEvent>& events, size_t nth = 0) {
    size_t count = 0;
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) {
            if (count == nth) return std::get<T>(e);
            ++count;
        }
    }
    throw std::runtime_error("Event not found");
}

template <typename T>
size_t countEvents(const std::vector<FeedEvent>& events) {
    size_t n = 0;
    for (const auto& e : events) {
        if (std::holds_alternative<T>(e)) ++n;
    }
    return n;
}

} // anonymous namespace

// ===========================================================================
// stock_directory
// ===========================================================================

TEST(ItchAdapterTest, StockDirectoryRegistersSymbolAndTickSize) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 42)
        .str("stock", "AAPL    ")   // ITCH pads to 8 chars
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(hasEvent<ObTickSizeEvent>(events));

    const auto& ts = getEvent<ObTickSizeEvent>(events);
    EXPECT_EQ(ts.symbol, "AAPL");          // trailing spaces trimmed
    EXPECT_DOUBLE_EQ(ts.tickSize, 0.01);   // NASDAQ default
}

TEST(ItchAdapterTest, StockDirectoryMissingFieldsProducesNoEvents) {
    ItchAdapter adapter;

    // Missing "stock" field
    auto msg = JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 1)
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// add_order / add_order_mpid
// ===========================================================================

TEST(ItchAdapterTest, AddOrderProducesObAddEvent) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "MSFT    ")
        .uint("orderRef", 100)
        .str("side", "B")
        .uint("shares", 500)
        .num("price", 350.25)
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    const auto& add = getEvent<ObAddEvent>(events);
    EXPECT_EQ(add.symbol, "MSFT");
    EXPECT_EQ(add.orderId, 100u);
    EXPECT_EQ(add.side, Side::Bid);
    EXPECT_DOUBLE_EQ(add.price, 350.25);
    EXPECT_EQ(add.size, 500u);
    EXPECT_EQ(add.priority, 0u);
}

TEST(ItchAdapterTest, AddOrderMpidRoutesSameAsAddOrder) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "add_order_mpid")
        .str("stock", "TSLA")
        .uint("orderRef", 200)
        .str("side", "S")
        .uint("shares", 100)
        .num("price", 180.0)
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(events.size(), 1u);
    const auto& add = getEvent<ObAddEvent>(events);
    EXPECT_EQ(add.side, Side::Ask);
}

TEST(ItchAdapterTest, AddOrderMissingFieldsProducesNoEvents) {
    ItchAdapter adapter;

    // Missing "shares"
    auto msg = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "X")
        .uint("orderRef", 1)
        .str("side", "B")
        .num("price", 10.0)
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// order_executed — full fill
// ===========================================================================

TEST(ItchAdapterTest, OrderExecutedFullFillDeletesAndEmitsTradeTick) {
    ItchAdapter adapter;

    // First add the order
    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "AAPL")
        .uint("orderRef", 10)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 150.0)
        .build();
    adapter.translate(add);

    // Execute the full amount
    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 10)
        .uint("shares", 100)
        .build();

    auto events = adapter.translate(exec);

    // Should produce: ObDeleteEvent + ObTradeEvent + TickEvent
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObTradeEvent>(events), 1u);
    EXPECT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.symbol, "AAPL");
    EXPECT_EQ(del.orderId, 10u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_DOUBLE_EQ(trade.price, 150.0);
    EXPECT_EQ(trade.size, 100u);
    EXPECT_EQ(trade.aggressor, Aggressor::Sell);  // Resting Bid → incoming Sell aggressor

    const auto& tick = getEvent<TickEvent>(events);
    EXPECT_EQ(tick.symbol, "AAPL");
    ASSERT_TRUE(tick.payload);
    EXPECT_TRUE(tick.payload->HasMember("lastPrice"));
    EXPECT_DOUBLE_EQ((*tick.payload)["lastPrice"].GetDouble(), 150.0);
}

// ===========================================================================
// order_executed — partial fill
// ===========================================================================

TEST(ItchAdapterTest, OrderExecutedPartialFillUpdatesAndEmitsTradeTick) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "GOOG")
        .uint("orderRef", 20)
        .str("side", "S")
        .uint("shares", 200)
        .num("price", 2800.0)
        .build();
    adapter.translate(add);

    // Partial fill: 50 of 200
    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 20)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(exec);

    // Should produce: ObUpdateEvent + ObTradeEvent + TickEvent (no delete)
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);
    EXPECT_EQ(countEvents<ObUpdateEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObTradeEvent>(events), 1u);
    EXPECT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.symbol, "GOOG");
    EXPECT_EQ(upd.orderId, 20u);
    ASSERT_TRUE(upd.newSize.has_value());
    EXPECT_EQ(*upd.newSize, 150u);   // 200 - 50
    EXPECT_FALSE(upd.newPrice.has_value());

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.aggressor, Aggressor::Buy);  // Resting Ask → incoming Buy aggressor
}

TEST(ItchAdapterTest, OrderExecutedUnknownOrderProducesNoEvents) {
    ItchAdapter adapter;

    auto exec = JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 999)
        .uint("shares", 10)
        .build();

    EXPECT_TRUE(adapter.translate(exec).empty());
}

// ===========================================================================
// order_cancel — partial and full
// ===========================================================================

TEST(ItchAdapterTest, OrderCancelPartialReducesSize) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "NFLX")
        .uint("orderRef", 30)
        .str("side", "B")
        .uint("shares", 300)
        .num("price", 450.0)
        .build();
    adapter.translate(add);

    auto cancel = JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 30)
        .uint("shares", 100)
        .build();

    auto events = adapter.translate(cancel);

    ASSERT_EQ(countEvents<ObUpdateEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObDeleteEvent>(events), 0u);

    const auto& upd = getEvent<ObUpdateEvent>(events);
    EXPECT_EQ(upd.symbol, "NFLX");
    ASSERT_TRUE(upd.newSize.has_value());
    EXPECT_EQ(*upd.newSize, 200u);  // 300 - 100
}

TEST(ItchAdapterTest, OrderCancelFullDeletesOrder) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "AMZN")
        .uint("orderRef", 40)
        .str("side", "S")
        .uint("shares", 50)
        .num("price", 3400.0)
        .build();
    adapter.translate(add);

    // Cancel all remaining
    auto cancel = JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 40)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(cancel);

    ASSERT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    EXPECT_EQ(countEvents<ObUpdateEvent>(events), 0u);
}

// ===========================================================================
// order_delete
// ===========================================================================

TEST(ItchAdapterTest, OrderDeleteRemovesOrder) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "META")
        .uint("orderRef", 50)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 300.0)
        .build();
    adapter.translate(add);

    auto del = JsonBuilder()
        .str("type", "order_delete")
        .uint("orderRef", 50)
        .build();

    auto events = adapter.translate(del);

    ASSERT_EQ(events.size(), 1u);
    const auto& d = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(d.symbol, "META");
    EXPECT_EQ(d.orderId, 50u);

    // Deleting again should produce nothing (order already gone)
    EXPECT_TRUE(adapter.translate(del).empty());
}

// ===========================================================================
// order_replace
// ===========================================================================

TEST(ItchAdapterTest, OrderReplaceDeletesOldAndAddsNew) {
    ItchAdapter adapter;

    auto add = JsonBuilder()
        .str("type", "add_order")
        .str("stock", "NVDA")
        .uint("orderRef", 60)
        .str("side", "B")
        .uint("shares", 100)
        .num("price", 500.0)
        .build();
    adapter.translate(add);

    auto replace = JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 60)
        .uint("orderRef", 61)
        .uint("shares", 200)
        .num("price", 505.0)
        .build();

    auto events = adapter.translate(replace);

    ASSERT_EQ(countEvents<ObDeleteEvent>(events), 1u);
    ASSERT_EQ(countEvents<ObAddEvent>(events), 1u);

    const auto& del = getEvent<ObDeleteEvent>(events);
    EXPECT_EQ(del.orderId, 60u);
    EXPECT_EQ(del.symbol, "NVDA");

    const auto& newAdd = getEvent<ObAddEvent>(events);
    EXPECT_EQ(newAdd.orderId, 61u);
    EXPECT_EQ(newAdd.symbol, "NVDA");
    EXPECT_EQ(newAdd.side, Side::Bid);   // preserves original side
    EXPECT_DOUBLE_EQ(newAdd.price, 505.0);
    EXPECT_EQ(newAdd.size, 200u);
}

TEST(ItchAdapterTest, OrderReplaceUnknownOrigProducesNoEvents) {
    ItchAdapter adapter;

    auto replace = JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 999)
        .uint("orderRef", 1000)
        .uint("shares", 100)
        .num("price", 10.0)
        .build();

    EXPECT_TRUE(adapter.translate(replace).empty());
}

// ===========================================================================
// trade
// ===========================================================================

TEST(ItchAdapterTest, TradeEmitsObTradeAndTickEvent) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "trade")
        .str("stock", "INTC    ")
        .num("price", 45.50)
        .uint("shares", 1000)
        .str("side", "B")
        .build();

    auto events = adapter.translate(msg);

    ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);
    ASSERT_EQ(countEvents<TickEvent>(events), 1u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.symbol, "INTC");
    EXPECT_DOUBLE_EQ(trade.price, 45.50);
    EXPECT_EQ(trade.size, 1000u);
    EXPECT_EQ(trade.aggressor, Aggressor::Buy);

    const auto& tick = getEvent<TickEvent>(events);
    EXPECT_EQ(tick.symbol, "INTC");
    ASSERT_TRUE(tick.payload);
    EXPECT_DOUBLE_EQ((*tick.payload)["lastPrice"].GetDouble(), 45.50);
    EXPECT_DOUBLE_EQ((*tick.payload)["volume"].GetDouble(), 1000.0);
}

TEST(ItchAdapterTest, TradeNoSideDefaultsToUnknownAggressor) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "trade")
        .str("stock", "AMD")
        .num("price", 120.0)
        .uint("shares", 50)
        .build();

    auto events = adapter.translate(msg);
    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_EQ(trade.aggressor, Aggressor::Unknown);
}

TEST(ItchAdapterTest, TradePriceAsStringParsesCorrectly) {
    ItchAdapter adapter;

    // ITCH sometimes sends price as string "185.2500"
    // Build this manually since JsonBuilder uses typed methods
    std::string msg = R"({"type":"trade","stock":"TEST","price":"185.2500","shares":10})";

    auto events = adapter.translate(msg);
    ASSERT_EQ(countEvents<ObTradeEvent>(events), 1u);

    const auto& trade = getEvent<ObTradeEvent>(events);
    EXPECT_DOUBLE_EQ(trade.price, 185.25);
}

// ===========================================================================
// system_event
// ===========================================================================

TEST(ItchAdapterTest, SystemEventProducesNoFeedEvents) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "system_event")
        .str("eventCode", "S")
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// Invalid / malformed messages
// ===========================================================================

TEST(ItchAdapterTest, InvalidJsonProducesNoEvents) {
    ItchAdapter adapter;
    EXPECT_TRUE(adapter.translate("not json at all").empty());
    EXPECT_TRUE(adapter.translate("{broken").empty());
    EXPECT_TRUE(adapter.translate("").empty());
}

TEST(ItchAdapterTest, MissingTypeFieldProducesNoEvents) {
    ItchAdapter adapter;
    EXPECT_TRUE(adapter.translate(R"({"stock":"AAPL"})").empty());
}

TEST(ItchAdapterTest, UnknownTypeProducesNoEvents) {
    ItchAdapter adapter;

    auto msg = JsonBuilder()
        .str("type", "unknown_message_type")
        .build();

    EXPECT_TRUE(adapter.translate(msg).empty());
}

// ===========================================================================
// Stateful: order lifecycle across multiple messages
// ===========================================================================

TEST(ItchAdapterTest, FullOrderLifecycle) {
    ItchAdapter adapter;

    // 1) Register symbol
    adapter.translate(JsonBuilder()
        .str("type", "stock_directory")
        .integer("stockLocate", 1)
        .str("stock", "LIFE    ")
        .build());

    // 2) Add order
    auto addEvents = adapter.translate(JsonBuilder()
        .str("type", "add_order")
        .str("stock", "LIFE")
        .uint("orderRef", 1)
        .str("side", "B")
        .uint("shares", 500)
        .num("price", 100.0)
        .build());
    ASSERT_EQ(countEvents<ObAddEvent>(addEvents), 1u);

    // 3) Partial fill: 200 of 500
    auto execEvents = adapter.translate(JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 1)
        .uint("shares", 200)
        .build());
    ASSERT_EQ(countEvents<ObUpdateEvent>(execEvents), 1u);
    EXPECT_EQ(getEvent<ObUpdateEvent>(execEvents).newSize.value(), 300u);

    // 4) Partial cancel: 100 of remaining 300
    auto cancelEvents = adapter.translate(JsonBuilder()
        .str("type", "order_cancel")
        .uint("orderRef", 1)
        .uint("shares", 100)
        .build());
    ASSERT_EQ(countEvents<ObUpdateEvent>(cancelEvents), 1u);
    EXPECT_EQ(getEvent<ObUpdateEvent>(cancelEvents).newSize.value(), 200u);

    // 5) Replace: remaining 200 → new order 2 at different price
    auto replaceEvents = adapter.translate(JsonBuilder()
        .str("type", "order_replace")
        .uint("origOrderRef", 1)
        .uint("orderRef", 2)
        .uint("shares", 200)
        .num("price", 101.0)
        .build());
    ASSERT_EQ(countEvents<ObDeleteEvent>(replaceEvents), 1u);
    ASSERT_EQ(countEvents<ObAddEvent>(replaceEvents), 1u);
    EXPECT_DOUBLE_EQ(getEvent<ObAddEvent>(replaceEvents).price, 101.0);

    // 6) Full fill of replacement
    auto fillEvents = adapter.translate(JsonBuilder()
        .str("type", "order_executed")
        .uint("orderRef", 2)
        .uint("shares", 200)
        .build());
    ASSERT_EQ(countEvents<ObDeleteEvent>(fillEvents), 1u);
    ASSERT_EQ(countEvents<ObTradeEvent>(fillEvents), 1u);
    ASSERT_EQ(countEvents<TickEvent>(fillEvents), 1u);

    // 7) Accessing deleted order produces nothing
    EXPECT_TRUE(adapter.translate(JsonBuilder()
        .str("type", "order_delete")
        .uint("orderRef", 2)
        .build()).empty());
}
