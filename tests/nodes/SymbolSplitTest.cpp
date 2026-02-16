#include "gma/nodes/SymbolSplit.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using namespace gma;

namespace {

class StubNode : public INode {
public:
    std::atomic<int> count{0};
    std::atomic<bool> shutdownCalled{false};
    void onValue(const SymbolValue& sv) override {
        (void)sv;
        count++;
    }
    void shutdown() noexcept override {
        shutdownCalled = true;
    }
};

} // anonymous namespace

TEST(SymbolSplitTest, FactoryCalledPerUniqueSymbol) {
    std::atomic<int> factoryCalls{0};
    std::mutex vecMutex;
    std::vector<std::weak_ptr<StubNode>> instances;

    auto factory = [&](const std::string& symbol) -> std::shared_ptr<INode> {
        (void)symbol;
        factoryCalls++;
        auto node = std::make_shared<StubNode>();
        {
            std::lock_guard<std::mutex> lock(vecMutex);
            instances.push_back(node);
        }
        return node;
    };

    SymbolSplit splitter(factory);
    splitter.onValue({"A", 1});
    splitter.onValue({"A", 2});
    splitter.onValue({"B", 3});
    splitter.onValue({"B", 4});
    splitter.onValue({"A", 5});

    EXPECT_EQ(factoryCalls.load(), 2);
    ASSERT_EQ(instances.size(), 2u);
}

TEST(SymbolSplitTest, OnValueForwardsToCorrectChild) {
    std::atomic<int> factoryCalls{0};
    std::shared_ptr<StubNode> nodeA, nodeB;

    auto factory = [&](const std::string& symbol) {
        factoryCalls++;
        if (symbol == "A") {
            nodeA = std::make_shared<StubNode>();
            return nodeA;
        } else {
            nodeB = std::make_shared<StubNode>();
            return nodeB;
        }
    };

    SymbolSplit splitter(factory);
    splitter.onValue({"A", 10});
    splitter.onValue({"A", 20});
    splitter.onValue({"B", 30});

    EXPECT_EQ(nodeA->count.load(), 2);
    EXPECT_EQ(nodeB->count.load(), 1);
}

TEST(SymbolSplitTest, ShutdownCallsChildShutdownAndClearsInstances) {
    auto nodeA = std::make_shared<StubNode>();
    auto nodeB = std::make_shared<StubNode>();
    auto factory = [&](const std::string& symbol) -> std::shared_ptr<INode> {
        return (symbol == "A") ? nodeA : nodeB;
    };
    SymbolSplit splitter(factory);
    splitter.onValue({"A", 1});
    splitter.onValue({"B", 1});
    splitter.shutdown();

    EXPECT_TRUE(nodeA->shutdownCalled.load());
    EXPECT_TRUE(nodeB->shutdownCalled.load());

    // After shutdown, instances cleared: new factory should be called for new split
    std::atomic<int> newCalls{0};
    auto factory2 = [&](const std::string& symbol) {
        (void)symbol;
        newCalls++;
        return std::make_shared<StubNode>();
    };
    SymbolSplit splitter2(factory2);
    splitter2.onValue({"X", 1});
    splitter2.shutdown();
    EXPECT_EQ(newCalls.load(), 1);
}

TEST(SymbolSplitTest, ConcurrencySafety) {
    std::atomic<int> factoryCalls{0};
    auto factory = [&](const std::string& symbol) {
        (void)symbol;
        factoryCalls++;
        return std::make_shared<StubNode>();
    };
    SymbolSplit splitter(factory);

    const int threads = 10;
    const int callsPerThread = 100;
    std::vector<std::thread> ths;
    for (int i = 0; i < threads; ++i) {
        ths.emplace_back([&](){
            for (int j = 0; j < callsPerThread; ++j) {
                splitter.onValue({"SYM", j});
            }
        });
    }
    for (auto& t : ths) t.join();

    EXPECT_EQ(factoryCalls.load(), 1);
}
