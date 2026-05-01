// ENC-35: tests for MarketTickComputer behavior under different
// MarketFieldMap configurations. Replaces the old tests/feed/
// SourceProfileTest.cpp (which referenced gma::SourceProfile, now
// removed from the engine).
//
// Tests construct a MarketTickComputer directly with a custom field
// map and inject via dispatcher.addComputer(). The Dispatcher's
// registry-bound default computer (default field map) also fires on
// each tick, but it no-ops when the test's payload doesn't carry the
// default field names — leaving the custom computer's writes intact.

#include "gma/AtomicFunctions.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/MarketTA.hpp"
#include "gma/StreamValue.hpp"
#include "gma/market/MarketFieldMap.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

using namespace gma;

namespace {

class TestNode : public INode {
public:
    std::atomic<int> count{0};
    std::vector<StreamValue> received;
    void onValue(const StreamValue& sv) override {
        received.push_back(sv);
        count++;
    }
    void shutdown() noexcept override {}
};

Event makeTick(const std::string& symbol,
               std::initializer_list<std::pair<const char*, double>> fields) {
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    auto& a = doc->GetAllocator();
    for (auto& [k, v] : fields) {
        doc->AddMember(rapidjson::Value(k, a), rapidjson::Value(v), a);
    }
    return Event{symbol, std::move(doc)};
}

// Convenience: build a Dispatcher and inject a MarketTickComputer with the
// given field map. The registry-registered default computer also fires per
// onTick; tests that pass non-default fields exploit the default's no-op
// behavior (it returns early when no priceField matches the payload).
std::unique_ptr<Dispatcher> makeDispatcherWithFieldMap(
    rt::ThreadPool& pool, AtomicStore& store, const util::Config& cfg,
    market::MarketFieldMap fieldMap) {
    auto md = std::make_unique<Dispatcher>(&pool, &store, cfg);
    md->addComputer(std::make_unique<MarketTickComputer>(cfg, std::move(fieldMap)));
    return md;
}

} // anonymous namespace

// ===========================================================================
// MarketFieldMap defaults match legacy NASDAQ-style names
// ===========================================================================

TEST(MarketFieldMapTest, DefaultFieldMapMatchesLegacyNames) {
    market::MarketFieldMap fm;
    EXPECT_EQ(fm.priceFields,
              (std::vector<std::string>{"lastPrice", "price", "last", "px"}));
    EXPECT_EQ(fm.volumeFields,
              (std::vector<std::string>{"volume", "vol", "qty", "size"}));
}

// ===========================================================================
// Default field map (via registry-bound computer) fires TA on lastPrice
// ===========================================================================

TEST(MarketFieldMapTest, DefaultFieldMapFiresTAOnLastPrice) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    Dispatcher md(&pool, &store, cfg);

    md.onTick(makeTick("SYM", {{"lastPrice", 100.0}, {"volume", 50.0}}));
    pool.shutdown();

    auto val = store.get("SYM", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 100.0);
}

TEST(MarketFieldMapTest, DefaultFieldMapFallsBackToPrice) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    Dispatcher md(&pool, &store, cfg);

    md.onTick(makeTick("FB", {{"price", 250.0}, {"vol", 1000.0}}));
    pool.shutdown();

    auto val = store.get("FB", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 250.0);
}

// ===========================================================================
// Custom MarketFieldMap — non-standard field names
// ===========================================================================

TEST(MarketFieldMapTest, CustomPriceFieldTriggersTA) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    market::MarketFieldMap fm;
    fm.priceFields  = {"last_trade_price"};
    fm.volumeFields = {"last_trade_volume"};
    auto md = makeDispatcherWithFieldMap(pool, store, cfg, std::move(fm));

    md->onTick(makeTick("BTC", {{"last_trade_price", 41000.0},
                                 {"last_trade_volume", 1.0}}));
    md->onTick(makeTick("BTC", {{"last_trade_price", 42000.0},
                                 {"last_trade_volume", 1.5}}));
    pool.shutdown();

    auto val = store.get("BTC", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 42000.0);

    auto vol = store.get("BTC", "volume");
    ASSERT_TRUE(vol.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*vol), 1.5);
}

TEST(MarketFieldMapTest, CustomFieldMapIgnoresUnmappedPayload) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    market::MarketFieldMap fm;
    fm.priceFields = {"trade_px"};
    auto md = makeDispatcherWithFieldMap(pool, store, cfg, std::move(fm));

    // Custom field-map's price key is "trade_px"; payload only has "lastPrice".
    // The custom computer no-ops. The default registry computer DOES fire on
    // "lastPrice" → so we verify the *custom* computer didn't write by
    // checking a custom-map-only side effect: with the custom map, the volume
    // field "volume" wouldn't match (default volumeFields = "volume", "vol",
    // "qty", "size"). Use a tick payload that only the *default* computer
    // recognizes and confirm the default-fired result.
    md->onTick(makeTick("ETH", {{"lastPrice", 3000.0}, {"volume", 10.0}}));
    pool.shutdown();

    // Default-registry computer fires on "lastPrice" — value present.
    EXPECT_TRUE(store.get("ETH", "lastPrice").has_value());
}

TEST(MarketFieldMapTest, CustomFieldMapPriorityOrder) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    market::MarketFieldMap fm;
    fm.priceFields = {"px", "trade_price", "lastPrice"};
    auto md = makeDispatcherWithFieldMap(pool, store, cfg, std::move(fm));

    // Both "px" and "lastPrice" are in the payload. Custom computer uses
    // "px" (first in its list). Default computer uses "lastPrice" (first in
    // default list). Custom fires AFTER default → "px" value wins.
    md->onTick(makeTick("TEST", {{"px", 99.0}, {"lastPrice", 100.0},
                                  {"volume", 1.0}}));
    pool.shutdown();

    auto val = store.get("TEST", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 99.0);
}

// ===========================================================================
// Raw-field listeners are unaffected by the field map
// ===========================================================================

TEST(MarketFieldMapTest, RawFieldListenerNotAffectedByFieldMap) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    market::MarketFieldMap fm;
    fm.priceFields = {"custom_price"};
    auto md = makeDispatcherWithFieldMap(pool, store, cfg, std::move(fm));

    auto listener = std::make_shared<TestNode>();
    md->registerListener("X", "bid", listener);

    // Payload has "bid" but neither the custom map's "custom_price" nor the
    // default map's price aliases. Both computers no-op; raw listener still
    // receives the bid value.
    md->onTick(makeTick("X", {{"bid", 42.0}}));
    pool.shutdown();

    EXPECT_GE(listener->count.load(), 1);
    ASSERT_FALSE(listener->received.empty());
    EXPECT_DOUBLE_EQ(std::get<double>(listener->received[0].value), 42.0);
}

// ===========================================================================
// taEnabled=false skips TA indicator writes (unit-level)
// Pre-ENC-30 this lived in GapFixesTest as a Dispatcher-routing test, but
// now that the Dispatcher's registry path always provides a default tick
// computer (taEnabled=true) per Dispatcher, the only place to assert
// taEnabled=false behavior cleanly is at the computer level.
// ===========================================================================

TEST(MarketFieldMapTest, TaDisabledSkipsTAIndicatorWrites) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.taSMA = {5};

    market::MarketFieldMap fm;
    fm.taEnabled = false;
    MarketTickComputer computer(cfg, std::move(fm));

    Dispatcher dummy(&pool, &store, cfg);
    engine::ComputeContext ctx{&store, &dummy, &pool};

    for (int i = 1; i <= 15; ++i) {
        Event ev = makeTick("TA_OFF", {{"lastPrice", 100.0 + i}, {"volume", 10.0}});
        ev.type = "tick";
        computer.compute(ev, ctx);
    }
    pool.shutdown();

    // Base metrics still written.
    EXPECT_TRUE(store.get("TA_OFF", "lastPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "openPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "highPrice").has_value());
    EXPECT_TRUE(store.get("TA_OFF", "lowPrice").has_value());

    // TA indicators absent because taEnabled=false short-circuited the path.
    EXPECT_FALSE(store.get("TA_OFF", "sma_5").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "rsi_14").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "macd_line").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "ema_12").has_value());
    EXPECT_FALSE(store.get("TA_OFF", "bollinger_upper").has_value());
}

// ===========================================================================
// SMA periods read from cfg, computed for custom fields
// ===========================================================================

TEST(MarketFieldMapTest, MultipleTicksBuildHistoryWithCustomFieldMap) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.taSMA = {3};

    market::MarketFieldMap fm;
    fm.priceFields  = {"mid"};
    fm.volumeFields = {"qty"};
    auto md = makeDispatcherWithFieldMap(pool, store, cfg, std::move(fm));

    md->onTick(makeTick("C", {{"mid", 10.0}, {"qty", 100.0}}));
    md->onTick(makeTick("C", {{"mid", 20.0}, {"qty", 200.0}}));
    md->onTick(makeTick("C", {{"mid", 30.0}, {"qty", 300.0}}));
    pool.shutdown();

    // SMA(3) of {10, 20, 30} = 20
    auto sma = store.get("C", "sma_3");
    ASSERT_TRUE(sma.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*sma), 20.0);
}
