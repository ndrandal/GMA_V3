#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>

using namespace gma;
using namespace gma::nodes;

namespace {

class DownstreamStub : public INode {
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

} // anonymous namespace

TEST(ListenerTest, ForwardsValueToDownstreamViaPool) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("SYM", "field", stub, &pool, &dispatcher);
    listener->start();

    listener->onValue(SymbolValue{"SYM", 1.0});
    listener->onValue(SymbolValue{"SYM", 2.0});
    listener->onValue(SymbolValue{"SYM", 3.0});

    pool.shutdown();

    ASSERT_EQ(stub->safeSize(), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[1].value), 2.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[2].value), 3.0);
}

TEST(ListenerTest, ShutdownStopsPropagation) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("X", "field", stub, &pool, &dispatcher);
    listener->start();

    listener->onValue(SymbolValue{"X", 10.0});
    pool.drain();
    EXPECT_EQ(stub->count.load(), 1);

    listener->shutdown();
    stub->received.clear();
    stub->count = 0;

    listener->onValue(SymbolValue{"X", 20.0});
    pool.shutdown();
    EXPECT_EQ(stub->count.load(), 0);
}

TEST(ListenerTest, SymbolAndFieldAccessors) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("AAPL", "price", stub, &pool, &dispatcher);

    EXPECT_EQ(listener->symbol(), "AAPL");
    EXPECT_EQ(listener->field(), "price");
    pool.shutdown();
}

TEST(ListenerTest, HandlesRapidDispatch) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    MarketDispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("R", "field", stub, &pool, &dispatcher);
    listener->start();

    const int sends = 50;
    for (int i = 0; i < sends; ++i) {
        listener->onValue(SymbolValue{"R", static_cast<double>(i)});
    }

    pool.shutdown();
    EXPECT_EQ(stub->safeSize(), static_cast<size_t>(sends));
}

TEST(ListenerTest, NoCrashOnDestructionWithoutStart) {
    {
        rt::ThreadPool pool(1);
        AtomicStore store;
        MarketDispatcher dispatcher(&pool, &store);
        auto stub = std::make_shared<DownstreamStub>();
        auto listener = std::make_shared<Listener>("E", "field", stub, &pool, &dispatcher);
        // Deliberately not calling start()
        listener->shutdown();
    }
    SUCCEED();
}
