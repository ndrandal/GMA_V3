#include "gma/nodes/Worker.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <numeric>

using namespace gma;

// Stub INode to record onValue calls
class StubNode : public INode {
public:
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override {
        received.push_back(sv);
    }
    void shutdown() noexcept override {}
};

TEST(WorkerTest, DoesNotPropagateOnSingleValue) {
    // Function that returns sum of inputs
    Worker::Function sumFn = [](Span<const ArgType> inputs) {
        double sum = 0;
        for (auto& v : inputs) sum += std::get<double>(v);
        return ArgType(sum);
    };
    std::vector<std::shared_ptr<INode>> children;
    auto stub = std::make_shared<StubNode>();
    children.push_back(stub);
    Worker worker(sumFn, children);

    // Single onValue should not trigger
    worker.onValue({"SYM", 1.0});
    EXPECT_TRUE(stub->received.empty());
}

TEST(WorkerTest, PropagatesAfterTwoValues) {
    Worker::Function sumFn = [](Span<const ArgType> inputs) {
        double sum = 0;
        for (auto& v : inputs) sum += std::get<double>(v);
        return ArgType(sum);
    };
    auto stub1 = std::make_shared<StubNode>();
    auto stub2 = std::make_shared<StubNode>();
    Worker worker(sumFn, {stub1, stub2});

    // Two inputs for same symbol
    worker.onValue({"A", 2.0});
    worker.onValue({"A", 3.0});

    // Both children should receive one call with sum 5.0
    ASSERT_EQ(stub1->received.size(), 1u);
    ASSERT_EQ(stub2->received.size(), 1u);
    EXPECT_EQ(stub1->received[0].symbol, "A");
    EXPECT_DOUBLE_EQ(std::get<double>(stub1->received[0].value), 5.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub2->received[0].value), 5.0);
}

TEST(WorkerTest, MultipleBatchesSeparateTriggers) {
    Worker::Function prodFn = [](Span<const ArgType> inputs) {
        double prod = 1;
        for (auto& v : inputs) prod *= std::get<double>(v);
        return ArgType(prod);
    };
    auto stub = std::make_shared<StubNode>();
    Worker worker(prodFn, {stub});

    // First batch: 2 * 3 = 6
    worker.onValue({"X", 2.0});
    worker.onValue({"X", 3.0});
    // Second batch: 4 * 5 = 20
    worker.onValue({"X", 4.0});
    worker.onValue({"X", 5.0});

    ASSERT_EQ(stub->received.size(), 2u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 6.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[1].value), 20.0);
}

TEST(WorkerTest, SeparateSymbolsIndependent) {
    Worker::Function countFn = [](Span<const ArgType> inputs) {
        return ArgType(static_cast<double>(inputs.size()));
    };
    auto stub = std::make_shared<StubNode>();
    Worker worker(countFn, {stub});

    // Two values for A
    worker.onValue({"A", 1.0});
    worker.onValue({"A", 1.0});
    // Two values for B
    worker.onValue({"B", 1.0});
    worker.onValue({"B", 1.0});

    // Expect two triggers: one for A, one for B
    ASSERT_EQ(stub->received.size(), 2u);
    EXPECT_EQ(stub->received[0].symbol, "A");
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 2.0);
    EXPECT_EQ(stub->received[1].symbol, "B");
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[1].value), 2.0);
}

TEST(WorkerTest, ShutdownPreventsFurtherPropagation) {
    Worker::Function dummyFn = [](Span<const ArgType> inputs) { return inputs.empty() ? ArgType(0.0) : inputs[0]; };
    auto stub = std::make_shared<StubNode>();
    Worker worker(dummyFn, {stub});

    // Trigger once
    worker.onValue({"SYM", 1.0});
    worker.onValue({"SYM", 2.0});
    ASSERT_EQ(stub->received.size(), 1u);

    // Shutdown and attempt more
    worker.shutdown();
    worker.onValue({"SYM", 3.0});
    worker.onValue({"SYM", 4.0});
    EXPECT_EQ(stub->received.size(), 1u);
}

TEST(WorkerTest, NoCrashWithEmptyChildren) {
    Worker::Function anyFn = [](Span<const ArgType>) { return ArgType(); };
    Worker worker(anyFn, {});
    // Two values should not crash even with no children
    EXPECT_NO_THROW({
        worker.onValue({"S", 1});
        worker.onValue({"S", 2});
    });
}
