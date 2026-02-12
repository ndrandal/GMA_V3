#include "gma/AtomicStore.hpp"
#include "gma/nodes/Aggregate.hpp"
#include "gma/nodes/Worker.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <memory>

using namespace gma;

// Stub terminal to capture pipeline output
class PipelineTerminal : public INode {
public:
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override { received.push_back(sv); }
    void shutdown() noexcept override {}
};

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

    // Send 2 values -> Aggregate triggers -> Worker computes
    agg.onValue(SymbolValue{"SYM", 10.0});
    agg.onValue(SymbolValue{"SYM", 20.0});

    // Aggregate forwards both values individually to Worker.
    // Worker applies fn on each immediately (size-1 span each time).
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
