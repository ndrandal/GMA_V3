#include "gma/ClientConnection.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/RequestRegistry.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <string>

using namespace gma;
using namespace rapidjson;
using std::string;

// Helper to serialize RapidJSON Document to std::string
static string toJson(const Document& doc) {
    StringBuffer sb;
    Writer<StringBuffer> writer(sb);
    doc.Accept(writer);
    return sb.GetString();
}

// Mock server interface to capture outgoing messages
class MockServer {
public:
    string lastMessage;
    void send(const string& msg) { lastMessage = msg; }
};

// TestConnection overrides ClientConnection::send to route through MockServer
class TestConnection : public ClientConnection {
public:
    MockServer server;
    ThreadPool pool{1};
    AtomicStore store;
    MarketDispatcher dispatcher{&pool, &store};
    RequestRegistry registry;
    ExecutionContext ctx{&store, &pool};

    // ClientConnection ctor: callback invoked on registry messages
    TestConnection()
        : ClientConnection(&ctx, &dispatcher, &registry,
            [&](int key, const SymbolValue& sv){
                // Echo back a response through the send() mechanism
                Document resp(kObjectType);
                resp.AddMember("type", "DispatchResponse", resp.GetAllocator());
                resp.AddMember("key", key, resp.GetAllocator());
                resp.AddMember("symbol", Value().SetString(sv.symbol.c_str(), (SizeType)sv.symbol.size()), resp.GetAllocator());
                // Value is double or int; assume double for simplicity
                resp.AddMember("value", std::get<double>(sv.value), resp.GetAllocator());
                send(toJson(resp));
            }
          ) {}

    // Override send to capture JSON messages
    void send(const string& message) {
        server.send(message);
    }
};

// --------- Test Cases ----------

TEST(ClientConnectionTest, AuthSuccess) {
    TestConnection conn;
    Document req(kObjectType);
    req.AddMember("type", "Auth", req.GetAllocator());
    req.AddMember("user", "test", req.GetAllocator());
    req.AddMember("pass", "pass", req.GetAllocator());
    conn.onMessage(toJson(req));
    auto out = conn.server.lastMessage;
    EXPECT_NE(out.find("\"type\":\"Response\""), string::npos);
}

TEST(ClientConnectionTest, AuthFailure) {
    TestConnection conn;
    Document req(kObjectType);
    req.AddMember("type", "Auth", req.GetAllocator());
    // missing user/pass
    conn.onMessage(toJson(req));
    auto out = conn.server.lastMessage;
    EXPECT_NE(out.find("\"type\":\"Error\""), string::npos);
}

TEST(ClientConnectionTest, UnknownType) {
    TestConnection conn;
    Document req(kObjectType);
    req.AddMember("type", "FooBar", req.GetAllocator());
    conn.onMessage(toJson(req));
    auto out = conn.server.lastMessage;
    EXPECT_NE(out.find("\"type\":\"Error\""), string::npos);
}

TEST(ClientConnectionTest, MalformedJson) {
    TestConnection conn;
    conn.onMessage("{ not valid json");
    auto out = conn.server.lastMessage;
    EXPECT_NE(out.find("\"type\":\"Error\""), string::npos);
    EXPECT_NE(out.find("Malformed JSON"), string::npos);
}

TEST(ClientConnectionTest, DispatchRequest) {
    TestConnection conn;
    // Simulate a Dispatch type message handled by ClientConnection
    Document req(kObjectType);
    req.AddMember("type", "Dispatch", req.GetAllocator());
    req.AddMember("symbol", "SYM", req.GetAllocator());
    req.AddMember("value", 42.0, req.GetAllocator());
    conn.onMessage(toJson(req));
    auto out = conn.server.lastMessage;
    // Our echo callback sends a DispatchResponse with matching key and symbol/value
    EXPECT_NE(out.find("\"type\":\"DispatchResponse\""), string::npos);
    EXPECT_NE(out.find("\"symbol\":\"SYM\""), string::npos);
    EXPECT_NE(out.find("\"value\":42"), string::npos);
}

TEST(ClientConnectionTest, RequestHandling) {
    TestConnection conn;
    Document req(kObjectType);
    req.AddMember("type", "Request", req.GetAllocator());
    req.AddMember("action", "Compute", req.GetAllocator());
    conn.onMessage(toJson(req));
    auto out = conn.server.lastMessage;
    // Expect a Response type with action echoed or generic Response
    EXPECT_NE(out.find("\"type\":\"Response\""), string::npos);
}
