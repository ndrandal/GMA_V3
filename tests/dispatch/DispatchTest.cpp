#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
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
    std::vector<StreamValue> received;
    void onValue(const StreamValue& sv) override {
        received.push_back(sv);
        count++;
    }
    void shutdown() noexcept override {}
};

// Helper to create a Event with a JSON payload containing a numeric field
static Event makeTick(const std::string& symbol,
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
    return Event{symbol, std::move(doc)};
}

TEST(DispatcherTest, RegisterAndNotifyListener) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("AAPL", "price", listener);

    md.onTick(makeTick("AAPL", "price", 150.0));
    pool.shutdown();

    EXPECT_GE(listener->count.load(), 1);
}

TEST(DispatcherTest, DifferentSymbolNotNotified) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("AAPL", "price", listener);

    md.onTick(makeTick("GOOG", "price", 2800.0));
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(DispatcherTest, UnregisterStopsNotification) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("SYM", "field", listener);
    md.unregisterListener("SYM", "field", listener);

    md.onTick(makeTick("SYM", "field", 1.0));
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(DispatcherTest, MultipleListenersSameSymbol) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    Dispatcher md(&pool, &store);

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
    md.onTick(Event{"X", std::move(doc)});
    pool.shutdown();

    EXPECT_GE(l1->count.load(), 1);
    EXPECT_GE(l2->count.load(), 1);
}
