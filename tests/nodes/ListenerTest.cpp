#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <atomic>

using namespace gma;
using namespace gma::nodes;

// Stub node to record received SymbolValues
class DownstreamStub : public INode {
public:
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override {
        received.push_back(sv);
    }
    void shutdown() noexcept override {}
};

TEST(ListenerTest, RegistersAndReceivesMatchingTicks) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("SYM", "field", stub, &pool, &dispatcher);

    // Dispatch several matching ticks
    dispatcher.onTick(SymbolValue{"SYM", 1});
    dispatcher.onTick(SymbolValue{"SYM", 2});
    dispatcher.onTick(SymbolValue{"SYM", 3});

    // Ensure all tasks complete
    pool.shutdown();

    ASSERT_EQ(stub->received.size(), 3u);
    EXPECT_EQ(stub->received[0].symbol, "SYM");
    EXPECT_EQ(std::get<int>(stub->received[0].value), 1);
    EXPECT_EQ(std::get<int>(stub->received[1].value), 2);
    EXPECT_EQ(std::get<int>(stub->received[2].value), 3);
}

TEST(ListenerTest, IgnoresNonMatchingSymbol) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("A", "field", stub, &pool, &dispatcher);

    dispatcher.onTick(SymbolValue{"B", 42});
    pool.shutdown();

    EXPECT_TRUE(stub->received.empty());
}

TEST(ListenerTest, ShutdownUnregistersAndStopsPropagation) {
    ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("X", "field", stub, &pool, &dispatcher);

    // Initial tick should propagate
    dispatcher.onTick(SymbolValue{"X", 10});
    pool.shutdown();
    EXPECT_EQ(stub->received.size(), 1u);

    // Shutdown listener and clear stub
    listener->shutdown();
    stub->received.clear();

    // Further ticks should not reach stub
    dispatcher.onTick(SymbolValue{"X", 20});
    pool.shutdown();
    EXPECT_TRUE(stub->received.empty());
}

TEST(ListenerTest, HandlesRapidDispatch) {
    ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("R", "field", stub, &pool, &dispatcher);

    const int sends = 50;
    for (int i = 0; i < sends; ++i) {
        dispatcher.onTick(SymbolValue{"R", i});
    }

    pool.shutdown();
    EXPECT_EQ(stub->received.size(), static_cast<size_t>(sends));
    // Check ordering
    for (int i = 0; i < sends; ++i) {
        EXPECT_EQ(std::get<int>(stub->received[i].value), i);
    }
}

TEST(ListenerTest, NoCrashOnEmptyCallbackBeforeRun) {
    // Even if no ticks sent, destructor should not crash
    {
        ThreadPool pool(1);
        AtomicStore store;
        MarketDispatcher dispatcher(&pool, &store);
        auto stub = std::make_shared<DownstreamStub>();
        auto listener = std::make_shared<Listener>("E", "field", stub, &pool, &dispatcher);
        listener->shutdown();
    }
    SUCCEED();
}
