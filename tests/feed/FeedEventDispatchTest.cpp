#include "gma/feed/FeedEvent.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using namespace gma;
using namespace gma::feed;

// ===========================================================================
// FeedEvent variant construction and access
// ===========================================================================

TEST(FeedEventTest, TickEventConstruction) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& a = payload->GetAllocator();
    payload->AddMember("price", 100.0, a);

    FeedEvent evt = TickEvent{"AAPL", payload};

    ASSERT_TRUE(std::holds_alternative<TickEvent>(evt));
    const auto& tick = std::get<TickEvent>(evt);
    EXPECT_EQ(tick.symbol, "AAPL");
    ASSERT_TRUE(tick.payload);
    EXPECT_DOUBLE_EQ((*tick.payload)["price"].GetDouble(), 100.0);
}

TEST(FeedEventTest, ObAddEventConstruction) {
    FeedEvent evt = ObAddEvent{"MSFT", 42, Side::Bid, 350.0, 100, 5};

    ASSERT_TRUE(std::holds_alternative<ObAddEvent>(evt));
    const auto& add = std::get<ObAddEvent>(evt);
    EXPECT_EQ(add.symbol, "MSFT");
    EXPECT_EQ(add.orderId, 42u);
    EXPECT_EQ(add.side, Side::Bid);
    EXPECT_DOUBLE_EQ(add.price, 350.0);
    EXPECT_EQ(add.size, 100u);
    EXPECT_EQ(add.priority, 5u);
}

TEST(FeedEventTest, ObAddEventDefaultPriority) {
    ObAddEvent add{"X", 1, Side::Ask, 10.0, 50};
    EXPECT_EQ(add.priority, 0u);
}

TEST(FeedEventTest, ObUpdateEventOptionalFields) {
    // Price only
    FeedEvent evt1 = ObUpdateEvent{"SYM", 1, 99.5, std::nullopt};
    const auto& u1 = std::get<ObUpdateEvent>(evt1);
    ASSERT_TRUE(u1.newPrice.has_value());
    EXPECT_DOUBLE_EQ(*u1.newPrice, 99.5);
    EXPECT_FALSE(u1.newSize.has_value());

    // Size only
    FeedEvent evt2 = ObUpdateEvent{"SYM", 2, std::nullopt, uint64_t{200}};
    const auto& u2 = std::get<ObUpdateEvent>(evt2);
    EXPECT_FALSE(u2.newPrice.has_value());
    ASSERT_TRUE(u2.newSize.has_value());
    EXPECT_EQ(*u2.newSize, 200u);

    // Both
    FeedEvent evt3 = ObUpdateEvent{"SYM", 3, 101.0, uint64_t{300}};
    const auto& u3 = std::get<ObUpdateEvent>(evt3);
    EXPECT_TRUE(u3.newPrice.has_value());
    EXPECT_TRUE(u3.newSize.has_value());
}

TEST(FeedEventTest, ObDeleteEventConstruction) {
    FeedEvent evt = ObDeleteEvent{"GOOG", 99};

    ASSERT_TRUE(std::holds_alternative<ObDeleteEvent>(evt));
    const auto& del = std::get<ObDeleteEvent>(evt);
    EXPECT_EQ(del.symbol, "GOOG");
    EXPECT_EQ(del.orderId, 99u);
}

TEST(FeedEventTest, ObTradeEventConstruction) {
    FeedEvent evt = ObTradeEvent{"TSLA", 180.0, 500, Aggressor::Sell};

    ASSERT_TRUE(std::holds_alternative<ObTradeEvent>(evt));
    const auto& t = std::get<ObTradeEvent>(evt);
    EXPECT_EQ(t.symbol, "TSLA");
    EXPECT_DOUBLE_EQ(t.price, 180.0);
    EXPECT_EQ(t.size, 500u);
    EXPECT_EQ(t.aggressor, Aggressor::Sell);
}

TEST(FeedEventTest, ObTradeEventDefaultAggressor) {
    ObTradeEvent t{"X", 10.0, 1};
    EXPECT_EQ(t.aggressor, Aggressor::Unknown);
}

TEST(FeedEventTest, ObTickSizeEventConstruction) {
    FeedEvent evt = ObTickSizeEvent{"NVDA", 0.01};

    ASSERT_TRUE(std::holds_alternative<ObTickSizeEvent>(evt));
    const auto& ts = std::get<ObTickSizeEvent>(evt);
    EXPECT_EQ(ts.symbol, "NVDA");
    EXPECT_DOUBLE_EQ(ts.tickSize, 0.01);
}

TEST(FeedEventTest, ObResetEventConstruction) {
    FeedEvent evt = ObResetEvent{"SPY", 7};

    ASSERT_TRUE(std::holds_alternative<ObResetEvent>(evt));
    const auto& r = std::get<ObResetEvent>(evt);
    EXPECT_EQ(r.symbol, "SPY");
    EXPECT_EQ(r.epoch, 7u);
}

TEST(FeedEventTest, ObResetEventDefaultEpoch) {
    ObResetEvent r{"X"};
    EXPECT_EQ(r.epoch, 0u);
}

// ===========================================================================
// std::visit dispatch covers all variants
// ===========================================================================

TEST(FeedEventTest, VisitCoversAllTypes) {
    std::vector<FeedEvent> events;
    events.push_back(TickEvent{"A", nullptr});
    events.push_back(ObAddEvent{"B", 1, Side::Bid, 1.0, 1});
    events.push_back(ObUpdateEvent{"C", 2, std::nullopt, uint64_t{1}});
    events.push_back(ObDeleteEvent{"D", 3});
    events.push_back(ObTradeEvent{"E", 1.0, 1});
    events.push_back(ObTickSizeEvent{"F", 0.01});
    events.push_back(ObResetEvent{"G", 0});

    int visited = 0;
    for (auto& evt : events) {
        std::visit([&](auto& e) {
            (void)e;
            visited++;
        }, evt);
    }

    EXPECT_EQ(visited, 7);
}

// ===========================================================================
// FeedEvent can be moved efficiently
// ===========================================================================

TEST(FeedEventTest, TickEventPayloadMovedNotCopied) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto raw = payload.get();

    FeedEvent evt = TickEvent{"X", std::move(payload)};
    EXPECT_EQ(payload, nullptr);  // moved out

    const auto& tick = std::get<TickEvent>(evt);
    EXPECT_EQ(tick.payload.get(), raw);  // same underlying object
}

// ===========================================================================
// Variant index stability (useful for binary serialization in the future)
// ===========================================================================

TEST(FeedEventTest, VariantIndexOrder) {
    EXPECT_EQ(FeedEvent(TickEvent{}).index(), 0u);
    EXPECT_EQ(FeedEvent(ObAddEvent{}).index(), 1u);
    EXPECT_EQ(FeedEvent(ObUpdateEvent{}).index(), 2u);
    EXPECT_EQ(FeedEvent(ObDeleteEvent{}).index(), 3u);
    EXPECT_EQ(FeedEvent(ObTradeEvent{}).index(), 4u);
    EXPECT_EQ(FeedEvent(ObTickSizeEvent{}).index(), 5u);
    EXPECT_EQ(FeedEvent(ObResetEvent{}).index(), 6u);
}
