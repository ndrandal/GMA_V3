#include "gma/MarketDispatcher.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <memory>
#include <atomic>

using namespace gma;

class TestListener : public INode {
public:
    std::atomic<int> count{0};
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override {
        received.push_back(sv);
        count++;
    }
    void shutdown() noexcept override {}
};

// Helper to create a SymbolTick with a JSON payload containing a numeric field
static SymbolTick makeTick(const std::string& symbol,
                           const std::string& field,
                           double value) {
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    auto& alloc = doc->GetAllocator();
    doc->AddMember(
        rapidjson::Value(field.c_str(), alloc),
        rapidjson::Value(value),
        alloc
    );
    return SymbolTick{symbol, std::move(doc)};
}

TEST(MarketDispatcherTest, RegisterAndNotifyListener) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("AAPL", "price", listener);

    md.onTick(makeTick("AAPL", "price", 150.0));
    pool.shutdown();

    EXPECT_GE(listener->count.load(), 1);
}

TEST(MarketDispatcherTest, DifferentSymbolNotNotified) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("AAPL", "price", listener);

    md.onTick(makeTick("GOOG", "price", 2800.0));
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(MarketDispatcherTest, UnregisterStopsNotification) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("SYM", "field", listener);
    md.unregisterListener("SYM", "field", listener);

    md.onTick(makeTick("SYM", "field", 1.0));
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(MarketDispatcherTest, MultipleListenersSameSymbol) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto l1 = std::make_shared<TestListener>();
    auto l2 = std::make_shared<TestListener>();
    md.registerListener("X", "price", l1);
    md.registerListener("X", "volume", l2);

    // Tick with both fields
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    auto& alloc = doc->GetAllocator();
    doc->AddMember("price", 100.0, alloc);
    doc->AddMember("volume", 5000.0, alloc);
    md.onTick(SymbolTick{"X", std::move(doc)});
    pool.shutdown();

    EXPECT_GE(l1->count.load(), 1);
    EXPECT_GE(l2->count.load(), 1);
}
