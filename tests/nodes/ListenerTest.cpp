#include "gma/nodes/Listener.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/StreamValue.hpp"
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
    std::vector<StreamValue> received;
    std::atomic<int> count{0};
    void onValue(const StreamValue& sv) override {
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
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("SYM", "field", stub, &pool, &dispatcher);
    listener->start();

    listener->onValue(StreamValue{"SYM", 1.0});
    listener->onValue(StreamValue{"SYM", 2.0});
    listener->onValue(StreamValue{"SYM", 3.0});

    pool.shutdown();

    ASSERT_EQ(stub->safeSize(), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[1].value), 2.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[2].value), 3.0);
}

TEST(ListenerTest, ShutdownStopsPropagation) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("X", "field", stub, &pool, &dispatcher);
    listener->start();

    listener->onValue(StreamValue{"X", 10.0});
    pool.drain();
    EXPECT_EQ(stub->count.load(), 1);

    listener->shutdown();
    stub->received.clear();
    stub->count = 0;

    listener->onValue(StreamValue{"X", 20.0});
    pool.shutdown();
    EXPECT_EQ(stub->count.load(), 0);
}

TEST(ListenerTest, SymbolAndFieldAccessors) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("AAPL", "price", stub, &pool, &dispatcher);

    EXPECT_EQ(listener->symbol(), "AAPL");
    EXPECT_EQ(listener->field(), "price");
    pool.shutdown();
}

TEST(ListenerTest, HandlesRapidDispatch) {
    rt::ThreadPool pool(2);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto listener = std::make_shared<Listener>("R", "field", stub, &pool, &dispatcher);
    listener->start();

    const int sends = 50;
    for (int i = 0; i < sends; ++i) {
        listener->onValue(StreamValue{"R", static_cast<double>(i)});
    }

    pool.shutdown();
    EXPECT_EQ(stub->safeSize(), static_cast<size_t>(sends));
}

TEST(ListenerTest, NoCrashOnDestructionWithoutStart) {
    {
        rt::ThreadPool pool(1);
        AtomicStore store;
        Dispatcher dispatcher(&pool, &store);
        auto stub = std::make_shared<DownstreamStub>();
        auto listener = std::make_shared<Listener>("E", "field", stub, &pool, &dispatcher);
        // Deliberately not calling start()
        listener->shutdown();
    }
    SUCCEED();
}

// --------------------------------------------------------------------
// Listener::Create factory — ENC-101 push-vs-pull rule enforcement.
// See GMA_V3/docs/atomic-keys.md and the spec at
// GMA_V3/specs/2026-05-06-ob-keys-pipeline-only/.
// --------------------------------------------------------------------

TEST(ListenerTest, RejectsObNamespaceAtFactory) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto res = Listener::Create("NEXO", "ob.best.bid.price", stub, &pool, &dispatcher);

    ASSERT_FALSE(res.has_value()) << "Listener::Create must reject ob.* fields";
    const auto& msg = res.error().message;
    EXPECT_NE(msg.find("pipeline-only"), std::string::npos)
        << "error message must contain literal 'pipeline-only'; got: " << msg;
    EXPECT_NE(msg.find("ob.best.bid.price"), std::string::npos)
        << "error message must echo the offending field name; got: " << msg;
    EXPECT_NE(msg.find("docs/atomic-keys.md"), std::string::npos)
        << "error message must point at docs/atomic-keys.md; got: " << msg;
    pool.shutdown();
}

TEST(ListenerTest, RejectsObSpread) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto res = Listener::Create("NEXO", "ob.spread", stub, &pool, &dispatcher);

    ASSERT_FALSE(res.has_value());
    EXPECT_NE(res.error().message.find("pipeline-only"), std::string::npos);
    EXPECT_NE(res.error().message.find("ob.spread"), std::string::npos);
    pool.shutdown();
}

TEST(ListenerTest, AcceptsBareKeyAtFactory) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto res = Listener::Create("AAPL", "lastPrice", stub, &pool, &dispatcher);

    ASSERT_TRUE(res.has_value()) << "bare key 'lastPrice' should pass";
    auto listener = res.value();
    EXPECT_EQ(listener->symbol(), "AAPL");
    EXPECT_EQ(listener->field(), "lastPrice");
    // Create() returns an already-started Listener; values propagate.
    listener->onValue(StreamValue{"AAPL", 42.0});
    pool.shutdown();
    EXPECT_EQ(stub->safeSize(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 42.0);
}

TEST(ListenerTest, FactoryDoesNotMatchObesityFalsePositive) {
    // Defensive: the prefix check is the literal three chars 'o','b','.'
    // — not a starts_with("ob") call. Field names like "obesity" or
    // "obvious" must NOT be rejected.
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher dispatcher(&pool, &store);

    auto stub = std::make_shared<DownstreamStub>();
    auto res = Listener::Create("X", "obesity", stub, &pool, &dispatcher);
    EXPECT_TRUE(res.has_value());
    pool.shutdown();
}
