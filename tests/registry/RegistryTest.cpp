#include "gma/RequestRegistry.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>
#include <string>

using namespace gma;

// Stub implementation to track shutdown calls
class StubNode : public INode {
public:
    std::atomic<bool> shutdownCalled{false};
    void onValue(const SymbolValue&) override {}  // Not used in registry
    void shutdown() noexcept override { shutdownCalled = true; }
};

TEST(RequestRegistryTest, RegisterAndShutdownAll) {
    RequestRegistry reg;
    auto node = std::make_shared<StubNode>();
    reg.registerRequest("id1", node);
    EXPECT_FALSE(node->shutdownCalled.load());
    reg.shutdownAll();
    EXPECT_TRUE(node->shutdownCalled.load());
}

TEST(RequestRegistryTest, UnregisterRequestCallsShutdownOnlyForThat) {
    RequestRegistry reg;
    auto node1 = std::make_shared<StubNode>();
    auto node2 = std::make_shared<StubNode>();
    reg.registerRequest("id1", node1);
    reg.registerRequest("id2", node2);
    reg.unregisterRequest("id1");
    EXPECT_TRUE(node1->shutdownCalled.load());
    EXPECT_FALSE(node2->shutdownCalled.load());
    reg.shutdownAll();
    EXPECT_TRUE(node2->shutdownCalled.load());
}

TEST(RequestRegistryTest, UnregisterNonExistentDoesNothing) {
    RequestRegistry reg;
    EXPECT_NO_THROW(reg.unregisterRequest("none"));
    EXPECT_NO_THROW(reg.shutdownAll());
}

TEST(RequestRegistryTest, OverwriteRegister) {
    RequestRegistry reg;
    auto node1 = std::make_shared<StubNode>();
    reg.registerRequest("id", node1);
    auto node2 = std::make_shared<StubNode>();
    reg.registerRequest("id", node2);
    reg.shutdownAll();
    EXPECT_FALSE(node1->shutdownCalled.load());
    EXPECT_TRUE(node2->shutdownCalled.load());
}

TEST(RequestRegistryTest, RegisterAfterShutdownAll) {
    RequestRegistry reg;
    auto node1 = std::make_shared<StubNode>();
    reg.registerRequest("id1", node1);
    reg.shutdownAll();
    EXPECT_TRUE(node1->shutdownCalled.load());
    auto node2 = std::make_shared<StubNode>();
    reg.registerRequest("id2", node2);
    reg.shutdownAll();
    EXPECT_TRUE(node2->shutdownCalled.load());
}

TEST(RequestRegistryTest, ConcurrencySafety) {
    RequestRegistry reg;
    const int count = 50;
    std::vector<std::shared_ptr<StubNode>> nodes;
    std::vector<std::thread> threads;
    for (int i = 0; i < count; ++i) {
        nodes.push_back(std::make_shared<StubNode>());
    }
    // Concurrent registration
    for (int i = 0; i < count; ++i) {
        threads.emplace_back([&reg, &nodes, i]() {
            reg.registerRequest("id" + std::to_string(i), nodes[i]);
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    reg.shutdownAll();
    for (int i = 0; i < count; ++i) {
        EXPECT_TRUE(nodes[i]->shutdownCalled.load());
    }
}
