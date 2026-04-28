#include "gma/Dispatcher.hpp"
#include "gma/SourceProfile.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/AtomicFunctions.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
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
// SourceProfile defaults match legacy behaviour
// ===========================================================================

TEST(SourceProfileTest, DefaultProfileMatchesLegacyFieldNames) {
    SourceProfile p;
    EXPECT_EQ(p.priceFields, (std::vector<std::string>{"lastPrice", "price", "last", "px"}));
    EXPECT_EQ(p.volumeFields, (std::vector<std::string>{"volume", "vol", "qty", "size"}));
}

// ===========================================================================
// Dispatcher uses SourceProfile for TA computation
// ===========================================================================

TEST(SourceProfileTest, DefaultProfileFiresTAOnLastPrice) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    Dispatcher md(&pool, &store, cfg);

    // "lastPrice" is the first default — TA should fire
    md.onTick(makeTick("SYM", {{"lastPrice", 100.0}, {"volume", 50.0}}));
    pool.shutdown();

    auto val = store.get("SYM", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 100.0);
}

TEST(SourceProfileTest, DefaultProfileFallsBackToPrice) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;

    Dispatcher md(&pool, &store, cfg);

    // "price" is the second fallback — TA should still fire
    md.onTick(makeTick("FB", {{"price", 250.0}, {"vol", 1000.0}}));
    pool.shutdown();

    auto val = store.get("FB", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 250.0);
}

// ===========================================================================
// Custom SourceProfile with non-standard field names
// ===========================================================================

TEST(SourceProfileTest, CustomPriceFieldTriggersTA) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"last_trade_price"};
    cfg.sourceProfile.volumeFields = {"last_trade_volume"};

    Dispatcher md(&pool, &store, cfg);

    // Send 2 ticks — computeAllAtomicValues returns early after n==1 before
    // storing volume metrics, so we need at least 2.
    md.onTick(makeTick("BTC", {{"last_trade_price", 41000.0},
                                {"last_trade_volume", 1.0}}));
    md.onTick(makeTick("BTC", {{"last_trade_price", 42000.0},
                                {"last_trade_volume", 1.5}}));
    pool.shutdown();

    // TA should have fired using the custom field name
    auto val = store.get("BTC", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 42000.0);

    auto vol = store.get("BTC", "volume");
    ASSERT_TRUE(vol.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*vol), 1.5);
}

TEST(SourceProfileTest, CustomProfileIgnoresUnmappedFields) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"trade_px"};

    Dispatcher md(&pool, &store, cfg);

    // Tick has "lastPrice" but profile only looks for "trade_px" — TA should NOT fire
    md.onTick(makeTick("ETH", {{"lastPrice", 3000.0}, {"volume", 10.0}}));
    pool.shutdown();

    EXPECT_FALSE(store.get("ETH", "lastPrice").has_value());
}

TEST(SourceProfileTest, CustomProfilePriorityOrder) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"px", "trade_price", "lastPrice"};

    Dispatcher md(&pool, &store, cfg);

    // Tick has both "px" and "lastPrice" — should use "px" (first in list)
    md.onTick(makeTick("TEST", {{"px", 99.0}, {"lastPrice", 100.0}, {"volume", 1.0}}));
    pool.shutdown();

    auto val = store.get("TEST", "lastPrice");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*val), 99.0);  // Used "px", not "lastPrice"
}

// ===========================================================================
// Raw field listeners still work regardless of SourceProfile
// ===========================================================================

TEST(SourceProfileTest, RawFieldListenerNotAffectedByProfile) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"custom_price"};

    Dispatcher md(&pool, &store, cfg);

    auto listener = std::make_shared<TestNode>();
    md.registerListener("X", "bid", listener);

    // The tick has "bid" but not "custom_price" — TA won't fire, but raw listener should
    md.onTick(makeTick("X", {{"bid", 42.0}}));
    pool.shutdown();

    EXPECT_GE(listener->count.load(), 1);
    ASSERT_FALSE(listener->received.empty());
    EXPECT_DOUBLE_EQ(std::get<double>(listener->received[0].value), 42.0);
}

// ===========================================================================
// Volume-only: price missing means no TA
// ===========================================================================

TEST(SourceProfileTest, NoMatchingPriceFieldSkipsTA) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"nonexistent_field"};

    Dispatcher md(&pool, &store, cfg);

    md.onTick(makeTick("SKIP", {{"lastPrice", 100.0}, {"volume", 50.0}}));
    pool.shutdown();

    // No TA should have run
    EXPECT_FALSE(store.get("SKIP", "lastPrice").has_value());
    EXPECT_FALSE(store.get("SKIP", "mean").has_value());
}

// ===========================================================================
// Dynamic skip list includes config-driven TA fields
// ===========================================================================

TEST(SourceProfileTest, DynamicSkipListIncludesConfiguredPeriods) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.taSMA = {7, 14};

    Dispatcher md(&pool, &store, cfg);

    // Feed enough ticks for sma_7 to compute
    for (int i = 0; i < 15; ++i) {
        md.onTick(makeTick("S", {{"lastPrice", 100.0 + i}, {"volume", 10.0}}));
    }
    pool.shutdown();

    // sma_7 and sma_14 should be in the store (computed by TA suite)
    EXPECT_TRUE(store.get("S", "sma_7").has_value());
    EXPECT_TRUE(store.get("S", "sma_14").has_value());
}

// ===========================================================================
// Multiple ticks accumulate history with custom profile
// ===========================================================================

TEST(SourceProfileTest, MultipleTicksBuildHistory) {
    registerBuiltinFunctions();
    rt::ThreadPool pool(1);
    AtomicStore store;
    util::Config cfg;
    cfg.sourceProfile.priceFields = {"mid"};
    cfg.sourceProfile.volumeFields = {"qty"};
    cfg.taSMA = {3};

    Dispatcher md(&pool, &store, cfg);

    md.onTick(makeTick("C", {{"mid", 10.0}, {"qty", 100.0}}));
    md.onTick(makeTick("C", {{"mid", 20.0}, {"qty", 200.0}}));
    md.onTick(makeTick("C", {{"mid", 30.0}, {"qty", 300.0}}));
    pool.shutdown();

    // SMA(3) of {10, 20, 30} = 20
    auto sma = store.get("C", "sma_3");
    ASSERT_TRUE(sma.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*sma), 20.0);

    // Mean of {10, 20, 30} = 20
    auto mean = store.get("C", "mean");
    ASSERT_TRUE(mean.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*mean), 20.0);
}
