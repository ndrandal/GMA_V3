#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace gma;

namespace {

// Downstream stub that records received values
class DownstreamStub : public INode {
public:
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override {
        received.push_back(sv);
    }
    void shutdown() noexcept override {}
};

} // anonymous namespace

TEST(AtomicAccessorTest, NoValueInStoreDoesNotPropagate) {
    AtomicStore store;
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", &store, downstream);
    accessor.onValue({"SYM", 123.0});
    EXPECT_TRUE(downstream->received.empty());
}

TEST(AtomicAccessorTest, PropagatesStoredValue) {
    AtomicStore store;
    store.set("SYM", "field", 3.14);
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", &store, downstream);
    accessor.onValue({"SYM", 0.0});
    ASSERT_EQ(downstream->received.size(), 1);
    EXPECT_EQ(downstream->received[0].symbol, "SYM");
    EXPECT_DOUBLE_EQ(std::get<double>(downstream->received[0].value), 3.14);
}

TEST(AtomicAccessorTest, UsesConfiguredSymbolNotInputSymbol) {
    AtomicStore store;
    store.set("A", "f", 42);
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("A", "f", &store, downstream);
    // Call with different SymbolValue symbol
    accessor.onValue({"B", 100});
    ASSERT_EQ(downstream->received.size(), 1);
    EXPECT_EQ(downstream->received[0].symbol, "A");
    EXPECT_EQ(std::get<int>(downstream->received[0].value), 42);
}

TEST(AtomicAccessorTest, ReflectsStoreUpdates) {
    AtomicStore store;
    store.set("SYM", "field", 1);
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", &store, downstream);
    accessor.onValue({"SYM", 0});
    store.set("SYM", "field", 2);
    accessor.onValue({"SYM", 0});
    ASSERT_EQ(downstream->received.size(), 2);
    EXPECT_EQ(std::get<int>(downstream->received[0].value), 1);
    EXPECT_EQ(std::get<int>(downstream->received[1].value), 2);
}

TEST(AtomicAccessorTest, MultiplePropagation) {
    AtomicStore store;
    store.set("SYM", "field", 7);
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", &store, downstream);
    for (int i = 0; i < 5; ++i) {
        accessor.onValue({"SYM", 0});
    }
    EXPECT_EQ(downstream->received.size(), 5);
}

TEST(AtomicAccessorTest, DifferentVariantTypes) {
    AtomicStore store;
    store.set("SYM", "iField", 10);
    store.set("SYM", "dField", 2.5);
    auto downstream1 = std::make_shared<DownstreamStub>();
    AtomicAccessor ai("SYM", "iField", &store, downstream1);
    ai.onValue({"SYM", 0});
    ASSERT_EQ(downstream1->received.size(), 1);
    EXPECT_EQ(std::get<int>(downstream1->received[0].value), 10);

    auto downstream2 = std::make_shared<DownstreamStub>();
    AtomicAccessor ad("SYM", "dField", &store, downstream2);
    ad.onValue({"SYM", 0.0});
    ASSERT_EQ(downstream2->received.size(), 1);
    EXPECT_DOUBLE_EQ(std::get<double>(downstream2->received[0].value), 2.5);
}

TEST(AtomicAccessorTest, ShutdownStopsPropagation) {
    AtomicStore store;
    store.set("SYM", "field", 99);
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", &store, downstream);
    accessor.shutdown();
    accessor.onValue({"SYM", 0});
    EXPECT_TRUE(downstream->received.empty());
}

TEST(AtomicAccessorTest, NoCrashOnShutdownWithoutStore) {
    // Even if store is nullptr, shutdown should not crash
    auto downstream = std::make_shared<DownstreamStub>();
    AtomicAccessor accessor("SYM", "field", nullptr, downstream);
    EXPECT_NO_THROW({ accessor.shutdown(); });
}
