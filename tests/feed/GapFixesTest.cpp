// tests/feed/GapFixesTest.cpp
//
// Tests for the 7 gap fixes:
//   Fix 1: Adapter-controlled subscribe message
//   Fix 2: Multi-feed config parsing
//   Fix 3: Configurable history limits
//   Fix 4: Opt-in TA computation
//   Fix 5+7: Richer TickEntry + timestamps
//   Fix 6: Allow negative prices in OrderBook

#include <gtest/gtest.h>

#include "gma/util/Config.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/AtomicFunctions.hpp"
#include "gma/Event.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/StreamValue.hpp"
#include "gma/SourceProfile.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/feed/IFeedAdapter.hpp"
#include "gma/feed/FeedEvent.hpp"
#include "gma/nodes/INode.hpp"

#include <rapidjson/document.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace gma;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

static Event makeTick(const std::string& symbol,
                           std::initializer_list<std::pair<const char*, double>> fields) {
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    auto& a = doc->GetAllocator();
    for (auto& [k, v] : fields) {
        doc->AddMember(rapidjson::Value(k, a), rapidjson::Value(v), a);
    }
    return Event{symbol, std::move(doc)};
}

} // anonymous namespace

// ===========================================================================
// Fix 3: Configurable history limits
// ===========================================================================

TEST(GapFixesTest, ConfigDefaultHistoryLimits) {
    util::Config cfg;
    EXPECT_EQ(cfg.taHistoryMax, 1000);
    EXPECT_EQ(cfg.maxSymbols, 10000);
    EXPECT_EQ(cfg.maxFieldsPerSymbol, 200);
}

TEST(GapFixesTest, MaxSymbolsLimitRejectsExcessSymbols) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.maxSymbols = 2;

    Dispatcher md(&pool, &store, cfg);

    // Send ticks for 3 symbols; the 3rd should be rejected by the symbol cap.
    md.onTick(makeTick("SYM_A", {{"lastPrice", 10.0}, {"volume", 1.0}}));
    md.onTick(makeTick("SYM_B", {{"lastPrice", 20.0}, {"volume", 1.0}}));
    md.onTick(makeTick("SYM_C", {{"lastPrice", 30.0}, {"volume", 1.0}}));
    pool.shutdown();

    // First two symbols should have their data in the store.
    EXPECT_TRUE(store.get("SYM_A", "lastPrice").has_value());
    EXPECT_TRUE(store.get("SYM_B", "lastPrice").has_value());

    // The third symbol should NOT have data — it was rejected.
    EXPECT_FALSE(store.get("SYM_C", "lastPrice").has_value());
}

// ===========================================================================
// Fix 6: Allow negative prices in OrderBook
// ===========================================================================

TEST(GapFixesTest, OrderBookRejectsNegativePricesByDefault) {
    OrderBookManager obm;
    obm.setTickSize("BOND", 0.01);

    // Negative price should be rejected when allowNegativePrices is false (default).
    EXPECT_FALSE(obm.onAdd("BOND", 1, Side::Bid, -1.00, 100, 0));
    EXPECT_FALSE(obm.bestBid("BOND").has_value());
}

TEST(GapFixesTest, OrderBookAllowsNegativePricesWhenEnabled) {
    OrderBookManager obm;
    obm.setTickSize("BOND", 0.01);
    obm.setAllowNegativePrices(true);

    // Negative price should now be accepted.
    EXPECT_TRUE(obm.onAdd("BOND", 1, Side::Bid, -1.00, 100, 0));

    auto bb = obm.bestBid("BOND");
    ASSERT_TRUE(bb.has_value());
    EXPECT_NEAR(*bb, -1.00, 1e-10);
}

TEST(GapFixesTest, OrderBookRejectsNanInfEvenWithNegativePrices) {
    OrderBookManager obm;
    obm.setTickSize("BOND", 0.01);
    obm.setAllowNegativePrices(true);

    EXPECT_FALSE(obm.onAdd("BOND", 1, Side::Bid,
                            std::numeric_limits<double>::quiet_NaN(), 100, 0));
    EXPECT_FALSE(obm.onAdd("BOND", 2, Side::Bid,
                            std::numeric_limits<double>::infinity(), 100, 0));
    EXPECT_FALSE(obm.onAdd("BOND", 3, Side::Bid,
                            -std::numeric_limits<double>::infinity(), 100, 0));

    // Book should still be empty.
    EXPECT_FALSE(obm.bestBid("BOND").has_value());
}

// ===========================================================================
// Fix 1: Adapter-controlled subscribe message
// ===========================================================================

TEST(GapFixesTest, DefaultAdapterSubscribeMessage) {
    // IFeedAdapter has a default subscribeMessage() implementation.
    // We need a concrete subclass to instantiate it since translate() is pure virtual.
    struct MinimalAdapter : public feed::IFeedAdapter {
        std::vector<feed::FeedEvent> translate(const std::string&) override {
            return {};
        }
    };

    MinimalAdapter adapter;
    std::string msg = adapter.subscribeMessage({"AAPL", "MSFT"});

    // Parse and verify the JSON structure.
    rapidjson::Document doc;
    doc.Parse(msg.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.IsObject());

    ASSERT_TRUE(doc.HasMember("action"));
    EXPECT_STREQ(doc["action"].GetString(), "subscribe");

    ASSERT_TRUE(doc.HasMember("symbols"));
    ASSERT_TRUE(doc["symbols"].IsArray());
    ASSERT_EQ(doc["symbols"].Size(), 2u);
    EXPECT_STREQ(doc["symbols"][0].GetString(), "AAPL");
    EXPECT_STREQ(doc["symbols"][1].GetString(), "MSFT");
}

TEST(GapFixesTest, CustomAdapterOverridesSubscribeMessage) {
    struct CustomAdapter : public feed::IFeedAdapter {
        std::vector<feed::FeedEvent> translate(const std::string&) override {
            return {};
        }
        std::string subscribeMessage(const std::vector<std::string>& symbols) override {
            // Custom format: pipe-separated symbols in a simple wrapper.
            std::string result = "{\"op\":\"sub\",\"channels\":\"";
            for (size_t i = 0; i < symbols.size(); ++i) {
                if (i > 0) result += "|";
                result += symbols[i];
            }
            result += "\"}";
            return result;
        }
    };

    CustomAdapter adapter;
    std::string msg = adapter.subscribeMessage({"BTC", "ETH"});

    rapidjson::Document doc;
    doc.Parse(msg.c_str());
    ASSERT_FALSE(doc.HasParseError());
    ASSERT_TRUE(doc.IsObject());

    ASSERT_TRUE(doc.HasMember("op"));
    EXPECT_STREQ(doc["op"].GetString(), "sub");

    ASSERT_TRUE(doc.HasMember("channels"));
    EXPECT_STREQ(doc["channels"].GetString(), "BTC|ETH");
}

// ===========================================================================
// Fix 5+7: Richer TickEntry + timestamps
// ===========================================================================

TEST(GapFixesTest, TickEntryDefaultFields) {
    TickEntry entry{100.0, 50.0};
    EXPECT_DOUBLE_EQ(entry.price, 100.0);
    EXPECT_DOUBLE_EQ(entry.volume, 50.0);
    EXPECT_DOUBLE_EQ(entry.bid, 0.0);
    EXPECT_DOUBLE_EQ(entry.ask, 0.0);
    EXPECT_EQ(entry.timestampNs, 0u);
}

TEST(GapFixesTest, TickEntryFullConstruction) {
    TickEntry entry{150.0, 200.0, 149.5, 150.5, 1700000000000000000ULL};
    EXPECT_DOUBLE_EQ(entry.price, 150.0);
    EXPECT_DOUBLE_EQ(entry.volume, 200.0);
    EXPECT_DOUBLE_EQ(entry.bid, 149.5);
    EXPECT_DOUBLE_EQ(entry.ask, 150.5);
    EXPECT_EQ(entry.timestampNs, 1700000000000000000ULL);
}

TEST(GapFixesTest, DispatcherStoresBidAskSpread) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.bidFields = {"bid"};
    cfg.sourceProfile.askFields = {"ask"};

    Dispatcher md(&pool, &store, cfg);

    // Send 2 ticks with bid/ask fields.
    md.onTick(makeTick("QQQ", {{"lastPrice", 400.0}, {"volume", 100.0},
                                {"bid", 399.5}, {"ask", 400.5}}));
    md.onTick(makeTick("QQQ", {{"lastPrice", 401.0}, {"volume", 200.0},
                                {"bid", 400.0}, {"ask", 401.5}}));
    pool.shutdown();

    // Verify bid, ask, spread are in the store (from the latest tick).
    auto bidVal = store.get("QQQ", "bid");
    ASSERT_TRUE(bidVal.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*bidVal), 400.0);

    auto askVal = store.get("QQQ", "ask");
    ASSERT_TRUE(askVal.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*askVal), 401.5);

    auto spreadVal = store.get("QQQ", "spread");
    ASSERT_TRUE(spreadVal.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*spreadVal), 1.5);  // 401.5 - 400.0
}

// ===========================================================================
// Fix 4: Opt-in TA computation
// ===========================================================================

TEST(GapFixesTest, TaDisabledStoresBaseMetricsOnly) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.taEnabled = false;
    cfg.taSMA = {5};

    Dispatcher md(&pool, &store, cfg);

    // Send 15 ticks — more than enough for SMA(5) and RSI(14) to compute.
    for (int i = 1; i <= 15; ++i) {
        md.onTick(makeTick("TA_OFF", {{"lastPrice", 100.0 + i}, {"volume", 10.0}}));
    }
    pool.shutdown();

    // Base metrics should be present.
    EXPECT_TRUE(store.get("TA_OFF", "lastPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "openPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "highPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "lowPrice").has_value());

    // Verify lastPrice value
    auto lp = store.get("TA_OFF", "lastPrice");
    ASSERT_TRUE(lp.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*lp), 115.0);  // last tick

    // Verify highPrice and lowPrice
    auto hp = store.get("TA_OFF", "highPrice");
    ASSERT_TRUE(hp.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*hp), 115.0);

    auto low = store.get("TA_OFF", "lowPrice");
    ASSERT_TRUE(low.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*low), 101.0);

    // TA indicators should NOT be present.
    EXPECT_FALSE(store.get("TA_OFF", "sma_5").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "rsi_14").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "macd_line").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "ema_12").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "bollinger_upper").has_value());
}

TEST(GapFixesTest, TaEnabledComputesIndicators) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.taEnabled = true;  // default, but explicit for clarity
    cfg.taSMA = {5};

    Dispatcher md(&pool, &store, cfg);

    // Send 15 ticks so SMA(5) can compute.
    for (int i = 1; i <= 15; ++i) {
        md.onTick(makeTick("TA_ON", {{"lastPrice", 100.0 + i}, {"volume", 10.0}}));
    }
    pool.shutdown();

    // Base metrics should be present.
    EXPECT_TRUE(store.get("TA_ON", "lastPrice").has_value());

    // TA indicator SMA(5) should be present.
    auto sma = store.get("TA_ON", "sma_5");
    ASSERT_TRUE(sma.has_value());
    // SMA(5) of last 5 prices: 111, 112, 113, 114, 115 => mean = 113
    EXPECT_DOUBLE_EQ(std::get<double>(*sma), 113.0);
}

// ===========================================================================
// Fix 2: Multi-feed config parsing
// ===========================================================================

TEST(GapFixesTest, MultiFeedConfigParsing) {
    const char* path = "test_gap_multifeed.ini";
    {
        std::ofstream f(path);
        f << "feed.0.url=wss://feed1.example.com/ws\n"
          << "feed.0.adapter=itch\n"
          << "feed.0.symbols=AAPL,MSFT,GOOG\n"
          << "feed.1.url=wss://feed2.example.com/ws\n"
          << "feed.1.adapter=generic\n"
          << "feed.1.symbols=BTC,ETH\n";
    }

    util::Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));

    ASSERT_EQ(cfg.feeds.size(), 2u);

    EXPECT_EQ(cfg.feeds[0].url, "wss://feed1.example.com/ws");
    EXPECT_EQ(cfg.feeds[0].adapter, "itch");
    EXPECT_EQ(cfg.feeds[0].symbols, (std::vector<std::string>{"AAPL", "MSFT", "GOOG"}));

    EXPECT_EQ(cfg.feeds[1].url, "wss://feed2.example.com/ws");
    EXPECT_EQ(cfg.feeds[1].adapter, "generic");
    EXPECT_EQ(cfg.feeds[1].symbols, (std::vector<std::string>{"BTC", "ETH"}));

    std::remove(path);
}

TEST(GapFixesTest, LegacyFeedFallbackWhenFeedsEmpty) {
    const char* path = "test_gap_legacy_feed.ini";
    {
        std::ofstream f(path);
        f << "feedUrl=wss://legacy.example.com/feed\n"
          << "feedSymbols=SPY,QQQ\n";
    }

    util::Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));

    // No feed.N.* keys present, so feeds vector should be empty.
    EXPECT_TRUE(cfg.feeds.empty());

    // Legacy fields should be populated.
    EXPECT_EQ(cfg.feedUrl, "wss://legacy.example.com/feed");
    EXPECT_EQ(cfg.feedSymbols, (std::vector<std::string>{"SPY", "QQQ"}));

    std::remove(path);
}

TEST(GapFixesTest, MultiFeedSingleEntry) {
    const char* path = "test_gap_single_feed.ini";
    {
        std::ofstream f(path);
        f << "feed.0.url=ws://localhost:8100/feed\n";
        // adapter and symbols not specified — should use defaults
    }

    util::Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));

    ASSERT_EQ(cfg.feeds.size(), 1u);
    EXPECT_EQ(cfg.feeds[0].url, "ws://localhost:8100/feed");
    EXPECT_EQ(cfg.feeds[0].adapter, "itch");  // default
    EXPECT_EQ(cfg.feeds[0].symbols, (std::vector<std::string>{"*"}));  // default

    std::remove(path);
}
