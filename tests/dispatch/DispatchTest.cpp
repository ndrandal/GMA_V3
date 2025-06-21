#include "gma/MarketDispatcher.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <deque>

using namespace gma;

// Simple listener stub implementing INode
class TestListener : public INode {
public:
    std::atomic<int> count{0};
    void onValue(const SymbolValue& sv) override {
        (void)sv;  // ignore content
        count++;
    }
    void shutdown() noexcept override {}
};

TEST(MarketDispatcherTest, SingleListener) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("A", "field", listener);
    md.onTick(SymbolValue{"A", 1.23});
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 1);
}

TEST(MarketDispatcherTest, MultipleListeners) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto l1 = std::make_shared<TestListener>();
    auto l2 = std::make_shared<TestListener>();
    md.registerListener("A", "f1", l1);
    md.registerListener("A", "f2", l2);
    md.onTick(SymbolValue{"A", 2.34});
    pool.shutdown();

    EXPECT_EQ(l1->count.load(), 1);
    EXPECT_EQ(l2->count.load(), 1);
}

TEST(MarketDispatcherTest, DifferentSymbols) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("A", "fld", listener);
    md.onTick(SymbolValue{"B", 3.45});
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(MarketDispatcherTest, UnregisterListener) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<TestListener>();
    md.registerListener("A", "fld", listener);
    md.unregisterListener("A", "fld", listener);
    md.onTick(SymbolValue{"A", 4.56});
    pool.shutdown();

    EXPECT_EQ(listener->count.load(), 0);
}

TEST(MarketDispatcherTest, HistoryCopyAndLimits) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    // Dispatch several ticks
    for (int i = 1; i <= 5; ++i) {
        md.onTick(SymbolValue{"SYM", static_cast<double>(i)});
    }
    pool.shutdown();

    auto history = md.getHistoryCopy("SYM");
    std::deque<double> expected{1.0, 2.0, 3.0, 4.0, 5.0};
    EXPECT_EQ(history, expected);
}

TEST(MarketDispatcherTest, UnseenSymbolHistoryEmpty) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher md(&pool, &store);

    auto history = md.getHistoryCopy("UNKNOWN");
    EXPECT_TRUE(history.empty());
}
