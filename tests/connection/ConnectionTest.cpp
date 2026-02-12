#include "gma/ws/ClientConnection.hpp"
#include "gma/RequestRegistry.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <memory>

using namespace gma;

// Test RequestRegistry as a stand-in for connection-level request management,
// since ClientConnection requires a running io_context and TCP connection.

class StubNode : public INode {
public:
    bool shutdownCalled = false;
    void onValue(const SymbolValue&) override {}
    void shutdown() noexcept override { shutdownCalled = true; }
};

TEST(ConnectionTest, RequestRegistryRegisterAndUnregister) {
    RequestRegistry registry;
    auto node = std::make_shared<StubNode>();

    registry.registerRequest("req-1", node);
    // Unregister should not crash
    registry.unregisterRequest("req-1");
    // Double unregister should be safe
    registry.unregisterRequest("req-1");
}

TEST(ConnectionTest, RequestRegistryShutdownAll) {
    RequestRegistry registry;
    auto n1 = std::make_shared<StubNode>();
    auto n2 = std::make_shared<StubNode>();

    registry.registerRequest("a", n1);
    registry.registerRequest("b", n2);

    registry.shutdownAll();
    EXPECT_TRUE(n1->shutdownCalled);
    EXPECT_TRUE(n2->shutdownCalled);
}

TEST(ConnectionTest, RequestRegistryShutdownAllTwiceIsSafe) {
    RequestRegistry registry;
    auto node = std::make_shared<StubNode>();
    registry.registerRequest("x", node);

    registry.shutdownAll();
    EXPECT_NO_THROW(registry.shutdownAll());
}

TEST(ConnectionTest, ClientConnectionTypeExists) {
    // Verify the ws::ClientConnection type compiles.
    // Full connection tests require a running io_context + TCP server.
    using CC = gma::ws::ClientConnection;
    using OnMsg = CC::OnMessage;
    static_assert(std::is_class_v<CC>, "ClientConnection should be a class");
    static_assert(std::is_invocable_v<OnMsg, const std::string&>,
                  "OnMessage should accept a string");
}
