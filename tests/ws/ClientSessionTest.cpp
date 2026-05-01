// ENC-48: live-socket coverage for ClientSession failure paths. Boots a
// real WebSocketServer on 127.0.0.1:0 and sends malformed / oversize /
// oversubscribed payloads through Beast, asserting the server emits the
// expected error frames.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/server/WebSocketServer.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ws    = beast::websocket;
using tcp       = asio::ip::tcp;

namespace {

struct ServerHarness {
  asio::io_context                    ioc;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::unique_ptr<gma::rt::ThreadPool> pool;
  std::unique_ptr<gma::AtomicStore>    store;
  std::unique_ptr<gma::Dispatcher>     dispatcher;
  std::unique_ptr<gma::ExecutionContext> exec;
  std::unique_ptr<gma::WebSocketServer>  server;
  std::thread                            ioThread;

  ServerHarness() {
    pool       = std::make_unique<gma::rt::ThreadPool>(1);
    store      = std::make_unique<gma::AtomicStore>();
    dispatcher = std::make_unique<gma::Dispatcher>(pool.get(), store.get());
    exec       = std::make_unique<gma::ExecutionContext>(store.get(), pool.get());
    server     = std::make_unique<gma::WebSocketServer>(ioc, exec.get(), dispatcher.get(), 0);
    server->run();
    work     = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
                 ioc.get_executor());
    ioThread = std::thread([this]{ ioc.run(); });
  }

  ~ServerHarness() {
    try { server->stopAccept(); } catch (...) {}
    try { server->closeAll();   } catch (...) {}
    if (work) work.reset();
    ioc.stop();
    if (ioThread.joinable()) ioThread.join();
  }

  unsigned short port() const { return server->port(); }
};

// Connect to the harness' WS endpoint. Returns a connected stream.
ws::stream<tcp::socket> connect(asio::io_context& clientIoc, unsigned short port) {
  tcp::resolver resolver(clientIoc);
  auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  ws::stream<tcp::socket> stream(clientIoc);
  asio::connect(stream.next_layer(), endpoints);
  stream.handshake("127.0.0.1", "/");
  return stream;
}

// Read one frame with a bounded wall-clock timeout. Returns empty on timeout.
// Uses a polling-cancel pattern so the call returns *as soon as* the frame
// arrives — no fixed timeout floor.
std::string readFrameBounded(ws::stream<tcp::socket>& stream,
                             std::chrono::milliseconds timeout) {
  std::atomic<bool> done{false};
  std::string out;
  auto deadline = std::chrono::steady_clock::now() + timeout;
  std::thread bomb([&]{
    while (!done.load()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        try { stream.next_layer().close(); } catch (...) {}
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  beast::flat_buffer buf;
  try {
    stream.read(buf);
    out = beast::buffers_to_string(buf.data());
  } catch (...) {}
  done.store(true);
  if (bomb.joinable()) bomb.join();
  return out;
}

// Parse a frame as a JSON object; ASSERT it has type=="error". Return the
// "where" and "message" fields.
struct ErrorFrame {
  std::string where;
  std::string message;
};

ErrorFrame expectErrorFrame(const std::string& payload) {
  ErrorFrame ef;
  rapidjson::Document doc;
  doc.Parse(payload.c_str());
  EXPECT_FALSE(doc.HasParseError()) << "frame is not JSON: " << payload;
  EXPECT_TRUE(doc.IsObject());
  EXPECT_TRUE(doc.HasMember("type"));
  if (doc.HasMember("type") && doc["type"].IsString()) {
    EXPECT_STREQ(doc["type"].GetString(), "error") << "frame: " << payload;
  }
  if (doc.HasMember("where") && doc["where"].IsString())
    ef.where = doc["where"].GetString();
  if (doc.HasMember("message") && doc["message"].IsString())
    ef.message = doc["message"].GetString();
  return ef;
}

} // namespace

TEST(ClientSessionTest, UnknownTypeReturnsError) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  stream.write(asio::buffer(R"({"type":"nope"})"));
  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty()) << "no frame received";

  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "type");
  EXPECT_NE(err.message.find("unknown type"), std::string::npos)
      << "message: " << err.message;

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, ParseErrorOnInvalidJson) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  stream.write(asio::buffer("{not json"));
  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty());

  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "parse");

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, SubscribeRejectsOversizedStreamKey) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  // MAX_STREAM_KEY_LEN is 64 in ClientSession.cpp.
  std::string oversized(80, 'X');
  std::string req =
    R"({"type":"subscribe","requests":[{"key":1,"streamKey":")" + oversized
    + R"(","field":"px"}]})";
  stream.write(asio::buffer(req));

  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty());
  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "subscribe");
  EXPECT_NE(err.message.find("streamKey"), std::string::npos)
      << "message: " << err.message;

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, SubscribeRejectsOversizedField) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  // MAX_FIELD_LEN is 128 in ClientSession.cpp.
  std::string oversized(150, 'F');
  std::string req =
    R"({"type":"subscribe","requests":[{"key":1,"streamKey":"X","field":")" + oversized
    + R"("}]})";
  stream.write(asio::buffer(req));

  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty());
  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "subscribe");
  EXPECT_NE(err.message.find("field"), std::string::npos)
      << "message: " << err.message;

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, CancelMissingKeysArrayError) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  stream.write(asio::buffer(R"({"type":"cancel"})"));
  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty());
  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "cancel");
  EXPECT_NE(err.message.find("keys"), std::string::npos);

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, CancelNonIntegerKeysError) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  stream.write(asio::buffer(R"({"type":"cancel","keys":["abc"]})"));
  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty());
  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "cancel");

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

TEST(ClientSessionTest, RateLimitEnforcedOnBurstSubscribes) {
  // RATE_LIMIT_BURST is 20 subscribe-batch invocations; the 21st should
  // produce a "rate limit exceeded" error frame.
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  // Send 22 subscribe messages back-to-back (each is one rate-limit token).
  // The first 20 are accepted. The 21st should be rate-limited.
  for (int i = 0; i < 22; ++i) {
    std::string req =
      std::string(R"({"type":"subscribe","requests":[{"key":)") + std::to_string(i) +
      R"(,"streamKey":"R","field":"px"}]})";
    stream.write(asio::buffer(req));
  }

  bool sawRateLimit = false;
  for (int i = 0; i < 30 && !sawRateLimit; ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(100));
    if (frame.empty()) break;
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (doc.HasParseError() || !doc.IsObject()) continue;
    if (doc.HasMember("type") && doc["type"].IsString() &&
        std::string(doc["type"].GetString()) == "error" &&
        doc.HasMember("message") && doc["message"].IsString()) {
      const std::string msg = doc["message"].GetString();
      if (msg.find("rate limit") != std::string::npos) {
        sawRateLimit = true;
      }
    }
  }

  EXPECT_TRUE(sawRateLimit)
      << "expected at least one 'rate limit exceeded' error after 22 bursts";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}
