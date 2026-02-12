#include "gma/nodes/Aggregate.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>

using namespace gma;

// Parent stub collects all received SymbolValues
class TestParent : public INode {
public:
    std::vector<SymbolValue> received;
    std::atomic<int> count{0};
    void onValue(const SymbolValue& sv) override {
        received.push_back(sv);
        ++count;
    }
    void shutdown() noexcept override {}
};

static double extractDouble(const ArgType& a) {
    return std::get<double>(a);
}

TEST(AggregateTest, TriggersAfterThreshold) {
    auto parent = std::make_shared<TestParent>();
    std::size_t N = 3;
    Aggregate agg(N, parent);

    std::vector<double> inputs{1.1, 2.2, 3.3};
    for (double v : inputs) {
        agg.onValue(SymbolValue{"SYM", v});
    }
    EXPECT_EQ(parent->count.load(), static_cast<int>(N));
    for (std::size_t i = 0; i < N; ++i) {
        EXPECT_EQ(parent->received[i].symbol, "SYM");
        EXPECT_DOUBLE_EQ(extractDouble(parent->received[i].value), inputs[i]);
    }
}

TEST(AggregateTest, ResetsAfterTrigger) {
    auto parent = std::make_shared<TestParent>();
    std::size_t N = 2;
    Aggregate agg(N, parent);

    agg.onValue(SymbolValue{"A", 10.0});
    agg.onValue(SymbolValue{"A", 20.0});
    EXPECT_EQ(parent->count.load(), 2);
    parent->received.clear(); parent->count = 0;

    agg.onValue(SymbolValue{"A", 30.0});
    agg.onValue(SymbolValue{"A", 40.0});
    EXPECT_EQ(parent->count.load(), 2);
    EXPECT_DOUBLE_EQ(extractDouble(parent->received[0].value), 30.0);
    EXPECT_DOUBLE_EQ(extractDouble(parent->received[1].value), 40.0);
}

TEST(AggregateTest, SeparateSymbolsIndependent) {
    auto parent = std::make_shared<TestParent>();
    std::size_t N = 2;
    Aggregate agg(N, parent);

    agg.onValue(SymbolValue{"X", 1.0});
    agg.onValue(SymbolValue{"X", 2.0});
    agg.onValue(SymbolValue{"Y", 3.0});
    agg.onValue(SymbolValue{"Y", 4.0});

    EXPECT_EQ(parent->count.load(), 4);
    EXPECT_EQ(parent->received[0].symbol, "X");
    EXPECT_EQ(parent->received[1].symbol, "X");
    EXPECT_EQ(parent->received[2].symbol, "Y");
    EXPECT_EQ(parent->received[3].symbol, "Y");
}

TEST(AggregateTest, ShutdownPreventsFurtherCallbacks) {
    auto parent = std::make_shared<TestParent>();
    Aggregate agg(1, parent);

    agg.onValue(SymbolValue{"Z", 5.0});
    EXPECT_EQ(parent->count.load(), 1);

    agg.shutdown();
    parent->received.clear(); parent->count = 0;
    agg.onValue(SymbolValue{"Z", 6.0});
    // After shutdown parent_ is reset, so no more callbacks
    EXPECT_EQ(parent->count.load(), 0);
}

TEST(AggregateTest, DoesNotTriggerBelowArity) {
    auto parent = std::make_shared<TestParent>();
    Aggregate agg(3, parent);

    agg.onValue(SymbolValue{"S", 1.0});
    agg.onValue(SymbolValue{"S", 2.0});
    // Only 2 of 3 received — should not trigger
    EXPECT_EQ(parent->count.load(), 0);
}

TEST(AggregateTest, ConcurrentOnValueIsSafe) {
    auto parent = std::make_shared<TestParent>();
    const std::size_t arity = 1; // trigger on every value for easy counting
    Aggregate agg(arity, parent);

    const int numThreads = 4;
    const int perThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&agg, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                agg.onValue(SymbolValue{"SYM_" + std::to_string(t),
                                        static_cast<double>(i)});
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(parent->count.load(), numThreads * perThread);
}
