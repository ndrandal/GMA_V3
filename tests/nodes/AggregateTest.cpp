#include "gma/nodes/Aggregate.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <atomic>

using namespace gma;

// Dummy child node; not actively used by Aggregate except for count
class DummyChild : public INode {
public:
    void onValue(const SymbolValue&) override {}
    void shutdown() noexcept override {}
};

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

// Helper to extract double from ArgType
static double extractDouble(const ArgType& a) {
    return std::get<double>(a);
}

TEST(AggregateTest, TriggersAfterThreshold) {
    auto parent = std::make_shared<TestParent>();
    size_t N = 3;
    std::vector<std::shared_ptr<INode>> children;
    for (size_t i = 0; i < N; ++i) children.push_back(std::make_shared<DummyChild>());
    Aggregate agg(children, parent);

    // Send N values; expect N callbacks
    std::vector<double> inputs{1.1, 2.2, 3.3};
    for (double v : inputs) {
        agg.onValue(SymbolValue{"SYM", v});
    }
    EXPECT_EQ(parent->count.load(), static_cast<int>(N));
    // Received values match inputs in order
    for (size_t i = 0; i < N; ++i) {
        EXPECT_EQ(parent->received[i].symbol, "SYM");
        EXPECT_DOUBLE_EQ(extractDouble(parent->received[i].value), inputs[i]);
    }
}

TEST(AggregateTest, ResetsAfterTrigger) {
    auto parent = std::make_shared<TestParent>();
    size_t N = 2;
    std::vector<std::shared_ptr<INode>> children(N, std::make_shared<DummyChild>());
    Aggregate agg(children, parent);

    // First batch
    agg.onValue(SymbolValue{"A", 10.0});
    agg.onValue(SymbolValue{"A", 20.0});
    EXPECT_EQ(parent->count.load(), 2);
    parent->received.clear(); parent->count = 0;

    // Second batch after reset
    agg.onValue(SymbolValue{"A", 30.0});
    agg.onValue(SymbolValue{"A", 40.0});
    EXPECT_EQ(parent->count.load(), 2);
    EXPECT_DOUBLE_EQ(extractDouble(parent->received[0].value), 30.0);
    EXPECT_DOUBLE_EQ(extractDouble(parent->received[1].value), 40.0);
}

TEST(AggregateTest, SeparateSymbolsIndependent) {
    auto parent = std::make_shared<TestParent>();
    size_t N = 2;
    std::vector<std::shared_ptr<INode>> children(N, std::make_shared<DummyChild>());
    Aggregate agg(children, parent);

    // Send for symbol X
    agg.onValue(SymbolValue{"X", 1.0});
    agg.onValue(SymbolValue{"X", 2.0});
    // Send for symbol Y
    agg.onValue(SymbolValue{"Y", 3.0});
    agg.onValue(SymbolValue{"Y", 4.0});

    // Expect 4 callbacks total
    EXPECT_EQ(parent->count.load(), 4);
    // Check ordering: X-values then Y-values
    EXPECT_EQ(parent->received[0].symbol, "X");
    EXPECT_EQ(parent->received[1].symbol, "X");
    EXPECT_EQ(parent->received[2].symbol, "Y");
    EXPECT_EQ(parent->received[3].symbol, "Y");
}

TEST(AggregateTest, ShutdownPreventsFurtherCallbacks) {
    auto parent = std::make_shared<TestParent>();
    size_t N = 1;
    std::vector<std::shared_ptr<INode>> children(N, std::make_shared<DummyChild>());
    Aggregate agg(children, parent);

    // Normal callback
    agg.onValue(SymbolValue{"Z", 5.0});
    EXPECT_EQ(parent->count.load(), 1);

    // Shutdown and attempt to send again
    agg.shutdown();
    parent->received.clear(); parent->count = 0;
    agg.onValue(SymbolValue{"Z", 6.0});
    EXPECT_EQ(parent->count.load(), 0);
}

TEST(AggregateTest, NoCrashWithEmptyChildren) {
    auto parent = std::make_shared<TestParent>();
    std::vector<std::shared_ptr<INode>> children;
    Aggregate agg(children, parent);
    // Sending any values should not crash; behavior undefined, but safe
    EXPECT_NO_THROW(agg.onValue(SymbolValue{"E", 0.0}));
}
