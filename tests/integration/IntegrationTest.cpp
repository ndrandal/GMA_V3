#include "gma/server/WebSocketServer.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/Config.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <thread>
#include <chrono>

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
using namespace rapidjson;

constexpr uint16_t TEST_PORT = 9002;

struct ServerFixture : public ::testing::Test {
    boost::asio::io_context ioc;
    std::unique_ptr<gma::ThreadPool> pool;
    std::unique_ptr<gma::AtomicStore> store;
    std::unique_ptr<gma::ExecutionContext> ctx;
    std::unique_ptr<gma::MarketDispatcher> dispatcher;
    std::unique_ptr<gma::WebSocketServer> server;
    std::thread serverThread;

    void SetUp() override {
        pool = std::make_unique<gma::ThreadPool>(gma::Config::ThreadPoolSize);
        store = std::make_unique<gma::AtomicStore>();
        dispatcher = std::make_unique<gma::MarketDispatcher>(pool.get(), store.get());
        ctx = std::make_unique<gma::ExecutionContext>(store.get(), pool.get());
        server = std::make_unique<gma::WebSocketServer>(ioc, ctx.get(), dispatcher.get(), TEST_PORT);
        server->run();
        serverThread = std::thread([&]{ ioc.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    void TearDown() override {
        ioc.stop();
        if (serverThread.joinable()) serverThread.join();
    }
};

static std::unique_ptr<websocket::stream<tcp::socket>> connectClient(boost::asio::io_context& ioc) {
    auto ws = std::make_unique<websocket::stream<tcp::socket>>(ioc);
    tcp::resolver resolver(ioc);
    auto results = resolver.resolve("127.0.0.1", std::to_string(TEST_PORT));
    boost::asio::connect(ws->next_layer(), results.begin(), results.end());
    ws->handshake("127.0.0.1", "/");
    return ws;
}

// Serialize a RapidJSON document to string
static std::string buildJson(const Document& doc) {
    StringBuffer buf;
    Writer<StringBuffer> writer(buf);
    doc.Accept(writer);
    return buf.GetString();
}

TEST_F(ServerFixture, MalformedJson) {
    auto ws = connectClient(ioc);
    ws->write(boost::asio::buffer(std::string("not a json")));

    boost::beast::flat_buffer buffer;
    ws->read(buffer);
    auto resp = boost::beast::buffers_to_string(buffer.data());

    EXPECT_NE(resp.find("\"type\":\"error\""), std::string::npos);
    EXPECT_NE(resp.find("Malformed JSON"), std::string::npos);
}

TEST_F(ServerFixture, MissingActionKey) {
    auto ws = connectClient(ioc);
    Document d(kObjectType);
    ws->write(boost::asio::buffer(buildJson(d)));

    boost::beast::flat_buffer buffer;
    ws->read(buffer);
    auto resp = boost::beast::buffers_to_string(buffer.data());

    EXPECT_NE(resp.find("Missing/invalid 'action' or 'key'"), std::string::npos);
}

TEST_F(ServerFixture, MissingRequest) {
    auto ws = connectClient(ioc);
    Document d(kObjectType);
    d.AddMember("action", StringRef("create"), d.GetAllocator());
    d.AddMember("key", 1, d.GetAllocator());
    ws->write(boost::asio::buffer(buildJson(d)));

    boost::beast::flat_buffer buffer;
    ws->read(buffer);
    auto resp = boost::beast::buffers_to_string(buffer.data());

    EXPECT_NE(resp.find("Missing/invalid 'request'"), std::string::npos);
}

TEST_F(ServerFixture, MissingRequestId) {
    auto ws = connectClient(ioc);
    Document d(kObjectType);
    Value req(kObjectType);
    req.AddMember("tree", Value(kObjectType).Move(), d.GetAllocator());
    d.AddMember("action", StringRef("create"), d.GetAllocator());
    d.AddMember("key", 2, d.GetAllocator());
    d.AddMember("request", req, d.GetAllocator());
    ws->write(boost::asio::buffer(buildJson(d)));

    boost::beast::flat_buffer buffer;
    ws->read(buffer);
    auto resp = boost::beast::buffers_to_string(buffer.data());

    EXPECT_NE(resp.find("Validation error: Request missing string id"), std::string::npos);
}

TEST_F(ServerFixture, SubscribeAndDispatch) {
    auto ws = connectClient(ioc);
    Document d(kObjectType);
    Value tree(kObjectType);
    tree.AddMember("type", StringRef("Listener"), d.GetAllocator());
    tree.AddMember("symbol", StringRef("SYM"), d.GetAllocator());
    tree.AddMember("field", StringRef("price"), d.GetAllocator());
    Value req(kObjectType);
    req.AddMember("id", StringRef("1"), d.GetAllocator());
    req.AddMember("symbol", StringRef("SYM"), d.GetAllocator());
    req.AddMember("field", StringRef("price"), d.GetAllocator());
    req.AddMember("tree", tree, d.GetAllocator());
    d.AddMember("action", StringRef("create"), d.GetAllocator());
    d.AddMember("key", 3, d.GetAllocator());
    d.AddMember("request", req, d.GetAllocator());
    ws->write(boost::asio::buffer(buildJson(d)));

    gma::SymbolValue sv{"SYM", 123.45};
    dispatcher->onTick(sv);

    boost::beast::flat_buffer buffer;
    ws->read(buffer);
    auto resp = boost::beast::buffers_to_string(buffer.data());

    EXPECT_NE(resp.find("\"type\":\"response\""), std::string::npos);
    EXPECT_NE(resp.find("\"key\":3"), std::string::npos);
    EXPECT_NE(resp.find("\"symbol\":\"SYM\""), std::string::npos);
    EXPECT_NE(resp.find("\"value\":123.45"), std::string::npos);
}
