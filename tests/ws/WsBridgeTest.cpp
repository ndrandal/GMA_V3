#include "gma/ws/WsBridge.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <mutex>

using namespace gma::ws;

// Capture all messages sent to a connection
struct Capture {
    std::mutex mu;
    std::vector<std::string> msgs;

    WsBridge::SendFn makeSend() {
        return [this](const std::string& msg) {
            std::lock_guard<std::mutex> lk(mu);
            msgs.push_back(msg);
        };
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.size();
    }

    std::string last() {
        std::lock_guard<std::mutex> lk(mu);
        return msgs.back();
    }
};

static std::unique_ptr<WsBridge> makeBridge() {
    // null dispatcher/store — we're only testing bridge protocol logic
    return std::make_unique<WsBridge>(nullptr, nullptr);
}

TEST(WsBridgeTest, OnOpenAndSubscribeAck) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onMessage("c1", R"({"type":"subscribe","symbol":"AAPL"})");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_NE(cap.last().find("ack"), std::string::npos);
}

TEST(WsBridgeTest, CancelAck) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onMessage("c1", R"({"type":"cancel"})");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_NE(cap.last().find("ack"), std::string::npos);
}

TEST(WsBridgeTest, InvalidJsonError) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onMessage("c1", "not valid json {{{");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_NE(cap.last().find("error"), std::string::npos);
    EXPECT_NE(cap.last().find("invalid JSON"), std::string::npos);
}

TEST(WsBridgeTest, MissingTypeError) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onMessage("c1", R"({"foo":"bar"})");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_NE(cap.last().find("missing"), std::string::npos);
}

TEST(WsBridgeTest, UnknownTypeError) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onMessage("c1", R"({"type":"foobar"})");
    ASSERT_EQ(cap.size(), 1u);
    EXPECT_NE(cap.last().find("unknown type"), std::string::npos);
}

TEST(WsBridgeTest, OnCloseRemovesConnection) {
    auto bridge = makeBridge();
    Capture cap;
    bridge->onOpen("c1", cap.makeSend());
    bridge->onClose("c1");
    // Messages after close should not be delivered
    bridge->onMessage("c1", R"({"type":"subscribe"})");
    EXPECT_EQ(cap.size(), 0u);
}

TEST(WsBridgeTest, MessageToUnknownConnNoCrash) {
    auto bridge = makeBridge();
    // Should not crash or throw
    EXPECT_NO_THROW(bridge->onMessage("unknown", R"({"type":"subscribe"})"));
}

TEST(WsBridgeTest, MultipleConnectionsIndependent) {
    auto bridge = makeBridge();
    Capture cap1, cap2;
    bridge->onOpen("c1", cap1.makeSend());
    bridge->onOpen("c2", cap2.makeSend());

    bridge->onMessage("c1", R"({"type":"subscribe"})");
    bridge->onMessage("c2", R"({"type":"cancel"})");

    EXPECT_EQ(cap1.size(), 1u);
    EXPECT_EQ(cap2.size(), 1u);
    EXPECT_NE(cap1.last().find("subscribe"), std::string::npos);
    EXPECT_NE(cap2.last().find("cancel"), std::string::npos);
}
