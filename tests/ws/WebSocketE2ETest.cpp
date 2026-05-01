// ENC-47: live-socket WebSocket end-to-end coverage. Boots a real
// WebSocketServer on 127.0.0.1:0, dials with a Beast WS client, and
// exercises the production accept / handshake / frame / close paths.
//
// Closes ADR-001 (chose WsBridge for Phase 1; now we have live-socket
// coverage too).

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

// Boot a WebSocketServer on 127.0.0.1:0 in a background thread; tear it
// down deterministically.
struct ServerHarness {
  asio::io_context                  ioc;
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
    work      = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
                  ioc.get_executor());
    ioThread  = std::thread([this]{ ioc.run(); });
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

} // namespace

// Plain WS connect → graceful close. Exercises the accept loop, handshake,
// and shutdown without touching the request handler.
TEST(WebSocketE2ETest, ConnectAndClose) {
  ServerHarness srv;
  ASSERT_GT(srv.port(), 0);

  asio::io_context clientIoc;
  tcp::resolver resolver(clientIoc);
  auto endpoints = resolver.resolve("127.0.0.1", std::to_string(srv.port()));

  ws::stream<tcp::socket> stream(clientIoc);
  asio::connect(stream.next_layer(), endpoints);
  stream.handshake("127.0.0.1", "/");

  beast::error_code closeEc;
  stream.close(ws::close_code::normal, closeEc);
  // close may return ec=closed if peer already closed; accept either.
  EXPECT_TRUE(!closeEc || closeEc == ws::error::closed
              || closeEc == beast::errc::operation_canceled
              || closeEc == asio::error::eof);
}

// Full pipeline round-trip: subscribe to a (symbol, field) pair, write a
// value into the AtomicStore on the server side, drive the listener via
// Dispatcher, read the response frame from the client. Exercises:
//   - subscribe handler validation
//   - JSON decode of the request
//   - Listener wiring through Responder
//   - JSON encode of the update message
//   - Beast frame write back to the client
TEST(WebSocketE2ETest, SubscribeAndReceiveUpdate) {
  // Engine builtins must be registered for the Listener / Responder
  // builders to resolve. Test bootstrap may have already done this; both
  // paths are idempotent for our purposes.
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  ServerHarness srv;
  ASSERT_GT(srv.port(), 0);

  asio::io_context clientIoc;
  tcp::resolver resolver(clientIoc);
  auto endpoints = resolver.resolve("127.0.0.1", std::to_string(srv.port()));

  ws::stream<tcp::socket> stream(clientIoc);
  asio::connect(stream.next_layer(), endpoints);
  stream.handshake("127.0.0.1", "/");

  // Send a minimal subscribe request.
  const std::string req =
    R"({"type":"subscribe","requests":[{"key":42,"symbol":"E2E","field":"px"}]})";
  stream.write(asio::buffer(req));

  // Give the server a moment to wire the listener.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Inject an event via the Dispatcher; listener should fire and the
  // server pushes an update frame back to the client.
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  doc->AddMember("px", 123.5, doc->GetAllocator());
  gma::Event ev;
  ev.type = "tick";
  ev.symbol = "E2E";
  ev.payload = std::move(doc);
  srv.dispatcher->onTick(ev);

  // Server emits a "subscribed" ack first, then "update" frames as the
  // listener fires. Read up to a few frames within a bounded timeout
  // looking for the update.
  std::atomic<bool> done{false};
  std::thread readerTimeout([&]{
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!done.load()) {
      try { stream.next_layer().close(); } catch (...) {}
    }
  });

  std::string updatePayload;
  for (int i = 0; i < 5 && updatePayload.empty(); ++i) {
    beast::flat_buffer buf;
    try {
      stream.read(buf);
    } catch (...) { break; }
    std::string frame = beast::buffers_to_string(buf.data());
    rapidjson::Document doc;
    doc.Parse(frame.c_str());
    if (!doc.HasParseError() && doc.IsObject() &&
        doc.HasMember("type") && doc["type"].IsString() &&
        std::string(doc["type"].GetString()) == "update") {
      updatePayload = std::move(frame);
    }
  }
  done.store(true);
  if (readerTimeout.joinable()) readerTimeout.join();

  ASSERT_FALSE(updatePayload.empty()) << "no update frame received within 2s";

  rapidjson::Document resp;
  resp.Parse(updatePayload.c_str());
  ASSERT_TRUE(resp.HasMember("key"));
  EXPECT_EQ(resp["key"].GetInt(), 42);

  beast::error_code closeEc;
  stream.close(ws::close_code::normal, closeEc);
}
