// ENC-48: live-socket coverage for ClientSession failure paths. Boots a
// real WebSocketServer on 127.0.0.1:0 and sends malformed / oversize /
// oversubscribed payloads through Beast, asserting the server emits the
// expected error frames.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
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

// ENC-101 push-vs-pull rule: a subscribe with field=ob.* must round-trip
// as an `error` frame whose `where` is "build" (TreeBuilder catch block at
// src/server/ClientSession.cpp ~line 510) and whose `message` contains
// the literal "pipeline-only" + the offending field. This is the only
// test that exercises the full WS round-trip; ListenerTest +
// TreeBuilderTest already cover the reject at lower layers.
TEST(ClientSessionTest, SubscribeRejectsObListenerField) {
  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  std::string req =
    R"({"type":"subscribe","requests":[{"key":1,"streamKey":"NEXO","field":"ob.best.bid.price"}]})";
  stream.write(asio::buffer(req));

  auto frame = readFrameBounded(stream, std::chrono::seconds(2));
  ASSERT_FALSE(frame.empty()) << "expected an error frame; got nothing";
  auto err = expectErrorFrame(frame);
  EXPECT_EQ(err.where, "build")
      << "ENC-101 reject is thrown from TreeBuilder, caught by the 'build' "
         "catch in ClientSession; got where=" << err.where;
  EXPECT_NE(err.message.find("pipeline-only"), std::string::npos)
      << "message must echo the canonical ENC-101 wording; got: " << err.message;
  EXPECT_NE(err.message.find("ob.best.bid.price"), std::string::npos)
      << "message must echo the offending field; got: " << err.message;

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

// gma-string-id-subscriptions phase 2: subscribe via the embassy-shaped
// {id:"<string>"} input. The ack must carry "requestId" (not "key"); the
// value frame must too. No "key" field in either path.
TEST(ClientSessionTest, SubscribeWithStringIdEchoesRequestId) {
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  const std::string req =
    R"({"type":"subscribe","requests":[{"id":"r-NEXO-test","streamKey":"STRID","field":"px"}]})";
  stream.write(asio::buffer(req));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto ev = std::make_shared<rapidjson::Document>();
  ev->SetObject();
  ev->AddMember("px", 7.5, ev->GetAllocator());
  gma::Event tick;
  tick.type = "tick";
  tick.symbol = "STRID";
  tick.payload = std::move(ev);
  srv.dispatcher->onTick(tick);

  std::string ackPayload, updatePayload;
  for (int i = 0; i < 8 && (ackPayload.empty() || updatePayload.empty()); ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(500));
    if (frame.empty()) break;
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("type")) continue;
    const std::string t = doc["type"].GetString();
    if (t == "subscribed" && ackPayload.empty()) ackPayload = frame;
    else if (t == "update" && updatePayload.empty()) updatePayload = frame;
  }

  ASSERT_FALSE(ackPayload.empty()) << "no 'subscribed' ack received";
  ASSERT_FALSE(updatePayload.empty()) << "no 'update' frame received";

  rapidjson::Document ack; ack.Parse(ackPayload.c_str());
  EXPECT_TRUE(ack.HasMember("requestId")) << "ack: " << ackPayload;
  EXPECT_FALSE(ack.HasMember("key")) << "ack must not have 'key' field for string-id sub: " << ackPayload;
  if (ack.HasMember("requestId") && ack["requestId"].IsString())
    EXPECT_STREQ(ack["requestId"].GetString(), "r-NEXO-test");

  rapidjson::Document upd; upd.Parse(updatePayload.c_str());
  EXPECT_TRUE(upd.HasMember("requestId")) << "update: " << updatePayload;
  EXPECT_FALSE(upd.HasMember("key")) << "update must not have 'key' field: " << updatePayload;
  if (upd.HasMember("requestId") && upd["requestId"].IsString())
    EXPECT_STREQ(upd["requestId"].GetString(), "r-NEXO-test");

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// Mixed int + string subscriptions on the same session. Each value frame
// must carry the field-shape that matches its subscription's input form.
TEST(ClientSessionTest, MixedIntAndStringSubsCoexist) {
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  // Two subscriptions on the same streamKey but different fields, so values
  // route to one or the other deterministically.
  stream.write(asio::buffer(
    R"({"type":"subscribe","requests":[{"key":1,"streamKey":"MIX","field":"px"}]})"));
  stream.write(asio::buffer(
    R"({"type":"subscribe","requests":[{"id":"alpha","streamKey":"MIX","field":"qty"}]})"));

  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  auto payload = std::make_shared<rapidjson::Document>();
  payload->SetObject();
  payload->AddMember("px", 11.0, payload->GetAllocator());
  payload->AddMember("qty", 22.0, payload->GetAllocator());
  gma::Event tick;
  tick.type = "tick";
  tick.symbol = "MIX";
  tick.payload = std::move(payload);
  srv.dispatcher->onTick(tick);

  bool sawIntFrame = false;
  bool sawStringFrame = false;
  for (int i = 0; i < 12 && (!sawIntFrame || !sawStringFrame); ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(500));
    if (frame.empty()) break;
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (doc.HasParseError() || !doc.IsObject()) continue;
    if (!doc.HasMember("type") || std::string(doc["type"].GetString()) != "update") continue;

    if (doc.HasMember("key") && doc["key"].IsInt() && doc["key"].GetInt() == 1) {
      sawIntFrame = true;
      EXPECT_FALSE(doc.HasMember("requestId")) << "int-keyed frame must not have requestId: " << frame;
    } else if (doc.HasMember("requestId") && doc["requestId"].IsString() &&
               std::string(doc["requestId"].GetString()) == "alpha") {
      sawStringFrame = true;
      EXPECT_FALSE(doc.HasMember("key")) << "string-keyed frame must not have key: " << frame;
    }
  }

  EXPECT_TRUE(sawIntFrame) << "no int-keyed update frame for key=1";
  EXPECT_TRUE(sawStringFrame) << "no string-keyed update frame for id=alpha";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// Cancel-by-ids removes the string-keyed subscription. After cancel, no
// further update frames should fire for that id.
TEST(ClientSessionTest, CancelByIdsRemovesStringKeyedSub) {
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  ServerHarness srv;
  asio::io_context clientIoc;
  auto stream = connect(clientIoc, srv.port());

  stream.write(asio::buffer(
    R"({"type":"subscribe","requests":[{"id":"foo","streamKey":"CANCEL","field":"px"}]})"));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Drain ack + at least one update to confirm the sub is live.
  auto ev1 = std::make_shared<rapidjson::Document>();
  ev1->SetObject();
  ev1->AddMember("px", 1.0, ev1->GetAllocator());
  gma::Event t1;
  t1.type = "tick"; t1.symbol = "CANCEL"; t1.payload = std::move(ev1);
  srv.dispatcher->onTick(t1);

  bool sawPreCancelUpdate = false;
  for (int i = 0; i < 6 && !sawPreCancelUpdate; ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(300));
    if (frame.empty()) break;
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("type") &&
        std::string(doc["type"].GetString()) == "update") {
      sawPreCancelUpdate = true;
    }
  }
  ASSERT_TRUE(sawPreCancelUpdate) << "string-id sub never fired even before cancel";

  // Cancel by ids array.
  stream.write(asio::buffer(R"({"type":"cancel","ids":["foo"]})"));
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  // Drain any in-flight ack frames so the next read window is clean.
  for (int i = 0; i < 4; ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(80));
    if (frame.empty()) break;
  }

  // Fire another tick — no update frame should follow.
  auto ev2 = std::make_shared<rapidjson::Document>();
  ev2->SetObject();
  ev2->AddMember("px", 2.0, ev2->GetAllocator());
  gma::Event t2;
  t2.type = "tick"; t2.symbol = "CANCEL"; t2.payload = std::move(ev2);
  srv.dispatcher->onTick(t2);

  bool sawPostCancelUpdate = false;
  for (int i = 0; i < 4; ++i) {
    auto frame = readFrameBounded(stream, std::chrono::milliseconds(200));
    if (frame.empty()) break;
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("type") &&
        std::string(doc["type"].GetString()) == "update" &&
        doc.HasMember("requestId") && doc["requestId"].IsString() &&
        std::string(doc["requestId"].GetString()) == "foo") {
      sawPostCancelUpdate = true;
    }
  }

  EXPECT_FALSE(sawPostCancelUpdate)
    << "received update frame for id='foo' after cancel by ids";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}
