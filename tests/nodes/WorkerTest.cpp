#include "gma/nodes/Worker.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

using namespace gma;

namespace {

// Stub INode to record onValue calls
class WStubNode : public INode {
public:
    std::mutex mx;
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override {
        std::lock_guard<std::mutex> lk(mx);
        received.push_back(sv);
    }
    void shutdown() noexcept override {}
};

} // anonymous namespace

// Worker currently applies fn immediately on every onValue and clears accumulator.
// Each call receives a Span of size 1 containing the latest value.

TEST(WorkerTest, PropagatesOnEveryValue) {
    Worker::Fn identityFn = [](Span<const ArgType> inputs) -> ArgType {
        if (inputs.empty()) return ArgType(0.0);
        return inputs[0];
    };
    auto stub = std::make_shared<WStubNode>();
    Worker worker(identityFn, stub);

    worker.onValue({"SYM", 1.0});
    worker.onValue({"SYM", 2.0});
    worker.onValue({"SYM", 3.0});

    ASSERT_EQ(stub->received.size(), 3u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 1.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[1].value), 2.0);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[2].value), 3.0);
}

TEST(WorkerTest, AppliesFunctionToValue) {
    Worker::Fn doubleFn = [](Span<const ArgType> inputs) -> ArgType {
        double sum = 0;
        for (auto& v : inputs) sum += std::get<double>(v);
        return ArgType(sum * 2.0);
    };
    auto stub = std::make_shared<WStubNode>();
    Worker worker(doubleFn, stub);

    worker.onValue({"A", 5.0});
    ASSERT_EQ(stub->received.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(stub->received[0].value), 10.0);
}

TEST(WorkerTest, SeparateSymbolsIndependent) {
    Worker::Fn countFn = [](Span<const ArgType> inputs) -> ArgType {
        return ArgType(static_cast<double>(inputs.size()));
    };
    auto stub = std::make_shared<WStubNode>();
    Worker worker(countFn, stub);

    worker.onValue({"A", 1.0});
    worker.onValue({"B", 1.0});

    ASSERT_EQ(stub->received.size(), 2u);
    EXPECT_EQ(stub->received[0].symbol, "A");
    EXPECT_EQ(stub->received[1].symbol, "B");
}

TEST(WorkerTest, ShutdownPreventsFurtherPropagation) {
    Worker::Fn passThrough = [](Span<const ArgType> inputs) -> ArgType {
        return inputs.empty() ? ArgType(0.0) : inputs[0];
    };
    auto stub = std::make_shared<WStubNode>();
    Worker worker(passThrough, stub);

    worker.onValue({"SYM", 1.0});
    ASSERT_EQ(stub->received.size(), 1u);

    worker.shutdown();
    worker.onValue({"SYM", 2.0});
    // downstream_ is reset after shutdown, so no more propagation
    EXPECT_EQ(stub->received.size(), 1u);
}

TEST(WorkerTest, NoCrashWithNullDownstream) {
    Worker::Fn anyFn = [](Span<const ArgType>) -> ArgType { return ArgType(0.0); };
    Worker worker(anyFn, nullptr);
    EXPECT_NO_THROW(worker.onValue({"S", 1.0}));
}

TEST(WorkerTest, ConcurrentOnValueIsSafe) {
    auto stub = std::make_shared<WStubNode>();
    std::atomic<int> callCount{0};
    Worker::Fn countFn = [&callCount](Span<const ArgType> inputs) -> ArgType {
        ++callCount;
        return inputs.empty() ? ArgType(0.0) : inputs[0];
    };
    Worker worker(countFn, stub);

    const int numThreads = 4;
    const int perThread = 100;
    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&worker, t, perThread]() {
            for (int i = 0; i < perThread; ++i) {
                worker.onValue({"SYM_" + std::to_string(t), static_cast<double>(i)});
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(callCount.load(), numThreads * perThread);
}
