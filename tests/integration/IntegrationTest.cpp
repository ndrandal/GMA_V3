#include "gma/AtomicStore.hpp"
#include "gma/AtomicFunctions.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/nodes/Aggregate.hpp"
#include "gma/nodes/Worker.hpp"
#include "gma/nodes/Listener.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/util/Config.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <fstream>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <rapidjson/document.h>

using namespace gma;

namespace {

// Thread-safe terminal to capture pipeline output
class PipelineTerminal : public INode {
public:
    std::mutex mx;
    std::vector<SymbolValue> received;
    std::atomic<int> count{0};
    void onValue(const SymbolValue& sv) override {
        {
            std::lock_guard<std::mutex> lk(mx);
            received.push_back(sv);
        }
        count.fetch_add(1, std::memory_order_release);
    }
    size_t safeSize() {
        std::lock_guard<std::mutex> lk(mx);
        return received.size();
    }
    void shutdown() noexcept override {}
};

// Helper: create a SymbolTick with a JSON payload
SymbolTick makeTick(const std::string& symbol, const std::string& field, double value) {
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    doc->AddMember(
        rapidjson::Value(field.c_str(), doc->GetAllocator()),
        rapidjson::Value(value),
        doc->GetAllocator()
    );
    return SymbolTick{symbol, doc};
}

} // anonymous namespace

// ============================================================
// Existing unit-level integration tests
// ============================================================

TEST(IntegrationTest, AggregateToWorkerPipeline) {
    // Wire: Aggregate(2) -> Worker(sum) -> Terminal
    auto terminal = std::make_shared<PipelineTerminal>();

    Worker::Fn sumFn = [](Span<const ArgType> inputs) -> ArgType {
        double s = 0;
        for (auto& v : inputs) s += std::get<double>(v);
        return ArgType(s);
    };
    auto worker = std::make_shared<Worker>(sumFn, terminal);
    Aggregate agg(2, worker);

    agg.onValue(SymbolValue{"SYM", 10.0});
    agg.onValue(SymbolValue{"SYM", 20.0});

    ASSERT_EQ(terminal->received.size(), 2u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 10.0);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[1].value), 20.0);
}

TEST(IntegrationTest, AtomicStoreBasicRoundTrip) {
    AtomicStore store;
    store.set("AAPL", "sma_20", 150.5);
    auto val = store.get("AAPL", "sma_20");
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(val.value()), 150.5);
}

TEST(IntegrationTest, AtomicStoreReturnsNulloptForMissing) {
    AtomicStore store;
    auto val = store.get("MISSING", "field");
    EXPECT_FALSE(val.has_value());
}

TEST(IntegrationTest, AtomicStoreBatchWrite) {
    AtomicStore store;
    std::vector<std::pair<std::string, ArgType>> batch = {
        {"sma_5",  100.0},
        {"sma_20", 105.0},
        {"rsi_14", 55.0},
    };
    store.setBatch("TEST", batch);

    auto v1 = store.get("TEST", "sma_5");
    auto v2 = store.get("TEST", "sma_20");
    auto v3 = store.get("TEST", "rsi_14");
    ASSERT_TRUE(v1.has_value());
    ASSERT_TRUE(v2.has_value());
    ASSERT_TRUE(v3.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*v1), 100.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*v2), 105.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*v3), 55.0);
}

TEST(IntegrationTest, ComputeAtomicValuesStoresResults) {
    SymbolHistory hist;
    for (int i = 1; i <= 25; ++i)
        hist.push_back({static_cast<double>(i), static_cast<double>(i * 2)});

    AtomicStore store;
    computeAllAtomicValues("INT_TEST", hist, store);

    EXPECT_TRUE(store.get("INT_TEST", "lastPrice").has_value());
    EXPECT_TRUE(store.get("INT_TEST", "openPrice").has_value());
    EXPECT_TRUE(store.get("INT_TEST", "highPrice").has_value());
    EXPECT_TRUE(store.get("INT_TEST", "lowPrice").has_value());
    EXPECT_TRUE(store.get("INT_TEST", "mean").has_value());
    EXPECT_TRUE(store.get("INT_TEST", "vwap").has_value());

    EXPECT_DOUBLE_EQ(std::get<double>(*store.get("INT_TEST", "lastPrice")), 25.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*store.get("INT_TEST", "openPrice")), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*store.get("INT_TEST", "highPrice")), 25.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*store.get("INT_TEST", "lowPrice")), 1.0);
}

TEST(IntegrationTest, MultipleSymbolsIndependent) {
    AtomicStore store;

    SymbolHistory hist1;
    for (int i = 1; i <= 10; ++i) hist1.push_back({static_cast<double>(i), 1.0});

    SymbolHistory hist2;
    for (int i = 100; i <= 110; ++i) hist2.push_back({static_cast<double>(i), 1.0});

    computeAllAtomicValues("SYM_A", hist1, store);
    computeAllAtomicValues("SYM_B", hist2, store);

    auto a = store.get("SYM_A", "lastPrice");
    auto b = store.get("SYM_B", "lastPrice");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*a), 10.0);
    EXPECT_DOUBLE_EQ(std::get<double>(*b), 110.0);

    EXPECT_FALSE(store.get("SYM_A", "nonexistent").has_value());
}

TEST(IntegrationTest, ConfigRoundTrip) {
    const char* path = "test_integration_cfg.ini";
    {
        std::ofstream f(path);
        f << "taSMA=3,7\n"
          << "taEMA=5\n"
          << "taRSI=5\n";
    }

    util::Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));

    SymbolHistory hist;
    for (int i = 1; i <= 25; ++i) hist.push_back({static_cast<double>(i), 1.0});

    AtomicStore store;
    computeAllAtomicValues("CFG_TEST", hist, store, cfg);

    EXPECT_TRUE(store.get("CFG_TEST", "sma_3").has_value());
    EXPECT_TRUE(store.get("CFG_TEST", "sma_7").has_value());
    EXPECT_TRUE(store.get("CFG_TEST", "ema_5").has_value());
    EXPECT_TRUE(store.get("CFG_TEST", "rsi_5").has_value());

    EXPECT_FALSE(store.get("CFG_TEST", "sma_5").has_value());
    EXPECT_FALSE(store.get("CFG_TEST", "sma_20").has_value());
    EXPECT_FALSE(store.get("CFG_TEST", "ema_12").has_value());
    EXPECT_FALSE(store.get("CFG_TEST", "rsi_14").has_value());

    std::remove(path);
}

// ============================================================
// End-to-end pipeline tests: tick → Listener → response
// ============================================================

TEST(IntegrationTest, TickToListenerToTerminal) {
    // Full pipeline: inject tick via MarketDispatcher → Listener forwards → terminal receives
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal = std::make_shared<PipelineTerminal>();
    auto listener = std::make_shared<nodes::Listener>("AAPL", "price", terminal, &pool, &dispatcher);
    listener->start();

    // Inject ticks
    dispatcher.onTick(makeTick("AAPL", "price", 150.0));
    dispatcher.onTick(makeTick("AAPL", "price", 151.5));
    dispatcher.onTick(makeTick("AAPL", "price", 149.0));

    pool.shutdown();

    ASSERT_EQ(terminal->safeSize(), 3u);
    // Values may arrive out of order due to thread pool — check all present
    std::vector<double> vals;
    for (auto& sv : terminal->received) vals.push_back(std::get<double>(sv.value));
    std::sort(vals.begin(), vals.end());
    EXPECT_DOUBLE_EQ(vals[0], 149.0);
    EXPECT_DOUBLE_EQ(vals[1], 150.0);
    EXPECT_DOUBLE_EQ(vals[2], 151.5);
}

TEST(IntegrationTest, TickToListenerToWorkerToTerminal) {
    // Full pipeline: tick → Listener → Worker(double) → Terminal
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal = std::make_shared<PipelineTerminal>();
    Worker::Fn doubleFn = [](Span<const ArgType> inputs) -> ArgType {
        if (inputs.empty()) return ArgType(0.0);
        return ArgType(std::get<double>(inputs[0]) * 2.0);
    };
    auto worker = std::make_shared<Worker>(doubleFn, terminal);

    auto listener = std::make_shared<nodes::Listener>("AAPL", "price", worker, &pool, &dispatcher);
    listener->start();

    dispatcher.onTick(makeTick("AAPL", "price", 100.0));
    dispatcher.onTick(makeTick("AAPL", "price", 200.0));

    pool.shutdown();

    ASSERT_EQ(terminal->safeSize(), 2u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 200.0);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[1].value), 400.0);
}

TEST(IntegrationTest, MultipleListenersSameSymbol) {
    // Two listeners on same (symbol, field) — both should receive ticks
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal1 = std::make_shared<PipelineTerminal>();
    auto terminal2 = std::make_shared<PipelineTerminal>();

    auto listener1 = std::make_shared<nodes::Listener>("AAPL", "price", terminal1, &pool, &dispatcher);
    auto listener2 = std::make_shared<nodes::Listener>("AAPL", "price", terminal2, &pool, &dispatcher);
    listener1->start();
    listener2->start();

    dispatcher.onTick(makeTick("AAPL", "price", 42.0));

    pool.shutdown();

    EXPECT_EQ(terminal1->safeSize(), 1u);
    EXPECT_EQ(terminal2->safeSize(), 1u);
}

TEST(IntegrationTest, ListenerIgnoresUnrelatedSymbols) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal = std::make_shared<PipelineTerminal>();
    auto listener = std::make_shared<nodes::Listener>("AAPL", "price", terminal, &pool, &dispatcher);
    listener->start();

    // Tick for different symbol — listener should not fire
    dispatcher.onTick(makeTick("GOOG", "price", 99.0));

    pool.shutdown();

    EXPECT_EQ(terminal->safeSize(), 0u);
}

TEST(IntegrationTest, ListenerIgnoresUnrelatedFields) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal = std::make_shared<PipelineTerminal>();
    auto listener = std::make_shared<nodes::Listener>("AAPL", "price", terminal, &pool, &dispatcher);
    listener->start();

    // Tick with different field — listener should not fire
    dispatcher.onTick(makeTick("AAPL", "volume", 1000.0));

    pool.shutdown();

    EXPECT_EQ(terminal->safeSize(), 0u);
}

TEST(IntegrationTest, AtomicAccessorReadsComputedValues) {
    // Compute atomics, then verify AtomicAccessor reads from store
    AtomicStore store;
    SymbolHistory hist;
    for (int i = 1; i <= 25; ++i) hist.push_back({static_cast<double>(i), 1.0});
    computeAllAtomicValues("SYM", hist, store);

    auto terminal = std::make_shared<PipelineTerminal>();
    AtomicAccessor accessor("SYM", "lastPrice", &store, terminal);

    // Trigger the accessor
    accessor.onValue(SymbolValue{"SYM", 0.0});

    ASSERT_EQ(terminal->received.size(), 1u);
    EXPECT_EQ(terminal->received[0].symbol, "SYM");
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 25.0);
}

TEST(IntegrationTest, ShutdownStopsEntirePipeline) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto terminal = std::make_shared<PipelineTerminal>();
    auto listener = std::make_shared<nodes::Listener>("S", "f", terminal, &pool, &dispatcher);
    listener->start();

    dispatcher.onTick(makeTick("S", "f", 1.0));
    pool.drain();
    EXPECT_GE(terminal->safeSize(), 1u);

    // Shutdown listener — no more forwarding
    listener->shutdown();
    size_t before = terminal->safeSize();

    dispatcher.onTick(makeTick("S", "f", 2.0));
    pool.drain();

    EXPECT_EQ(terminal->safeSize(), before);
    pool.shutdown();
}
