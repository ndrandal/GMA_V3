// ENC-996: outbox flow control for ClientSession.
//
// Before this ticket the outbox was a plain FIFO capped at MAX_OUTBOX_SIZE
// (4096) and the overflow policy was "kill the consumer": sendText() logged
// `ws.outbox_overflow` and called close(). A replay/backfill that produced
// values faster than the browser could drain them therefore *disconnected the
// browser* — an exact cliff, not a gradient (4096 queued frames fine, 4097
// fatal).
//
// These tests drive a real socket and a real WebSocketServer, flood a live
// subscription with far more updates than the client drains, and assert:
//   * the session survives (no close frame, no `ws.outbox_overflow`),
//   * the newest value still arrives (coalesce-latest keeps the head of the
//     stream, drops superseded intermediates),
//   * control frames (subscribed / canceled / error) are never coalesced away,
//   * a burst that stays under COALESCE_WATERMARK is still delivered in full.
//
// They fail on master: the flood closes the connection, so the "newest value"
// read comes back as websocket::error::closed instead.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/util/Metrics.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ws    = beast::websocket;
using tcp       = asio::ip::tcp;

namespace {

// Mirrors tests/ws/ClientSessionTest.cpp's harness. Kept local so the two
// files stay independently readable and neither one's tuning leaks into the
// other.
struct BurstHarness {
  asio::io_context ioc;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::unique_ptr<gma::rt::ThreadPool>   pool;
  std::unique_ptr<gma::AtomicStore>      store;
  std::unique_ptr<gma::Dispatcher>       dispatcher;
  std::unique_ptr<gma::ExecutionContext> exec;
  std::unique_ptr<gma::WebSocketServer>  server;
  std::thread                            ioThread;

  BurstHarness() {
    gma::registerBuiltinFunctions();
    gma::registerBuiltinNodeTypes();
    pool       = std::make_unique<gma::rt::ThreadPool>(1);
    store      = std::make_unique<gma::AtomicStore>();
    dispatcher = std::make_unique<gma::Dispatcher>(pool.get(), store.get());
    exec       = std::make_unique<gma::ExecutionContext>(store.get(), pool.get());
    server     = std::make_unique<gma::WebSocketServer>(ioc, exec.get(), dispatcher.get(), 0);
    server->run();
    work = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
             ioc.get_executor());
    ioThread = std::thread([this] { ioc.run(); });
  }

  ~BurstHarness() {
    try { server->stopAccept(); } catch (...) {}
    try { server->closeAll();   } catch (...) {}
    if (work) work.reset();
    ioc.stop();
    if (ioThread.joinable()) ioThread.join();
  }

  unsigned short port() const { return server->port(); }
};

// Connect with a deliberately tiny receive buffer. Without this the kernel
// socket buffers can silently absorb thousands of small frames and the server
// outbox never reaches its bound, which would make the boundary tests depend
// on the host's tcp_rmem autotuning rather than on the server's policy.
ws::stream<tcp::socket> connectSlowReader(asio::io_context& clientIoc,
                                          unsigned short port) {
  tcp::resolver resolver(clientIoc);
  auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port));
  ws::stream<tcp::socket> stream(clientIoc);
  asio::connect(stream.next_layer(), endpoints);
  stream.next_layer().set_option(asio::socket_base::receive_buffer_size(2048));
  stream.handshake("127.0.0.1", "/");
  return stream;
}

struct DrainResult {
  std::size_t frames      = 0;   // every frame read
  std::size_t updates     = 0;   // type == "update"
  double      lastValue   = std::nan("");
  bool        sawTarget   = false;
  bool        monotonic   = true;   // update values never go backwards
  bool        closed      = false;  // server tore the connection down
  bool        timedOut    = false;
  std::string closeDetail;
};

// Read frames until `target` shows up as an update value, the server closes,
// or the budget runs out. Single-threaded (async_read + run_for) so no second
// thread ever touches the socket — same pattern as ClientSessionTest.
DrainResult drainUntilValue(ws::stream<tcp::socket>& stream,
                            double target,
                            std::chrono::milliseconds budget) {
  auto& ioc = static_cast<asio::io_context&>(stream.get_executor().context());
  const auto deadline = std::chrono::steady_clock::now() + budget;
  DrainResult r;

  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) { r.timedOut = true; break; }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

    beast::flat_buffer buf;
    std::string frame;
    bool completed = false;
    beast::error_code readEc;
    stream.async_read(buf, [&](beast::error_code ec, std::size_t) {
      completed = true;
      readEc    = ec;
      if (!ec) frame = beast::buffers_to_string(buf.data());
    });
    ioc.restart();
    ioc.run_for(remaining);

    if (!completed) {  // budget elapsed with a read still pending
      beast::error_code ec;
      stream.next_layer().cancel(ec);
      ioc.restart();
      ioc.run_for(std::chrono::milliseconds(200));
      r.timedOut = true;
      break;
    }
    if (readEc) {
      r.closed      = true;
      r.closeDetail = readEc.message();
      if (readEc == ws::error::closed) {
        r.closeDetail += " reason='" + std::string(stream.reason().reason.c_str()) +
                         "' code=" + std::to_string(static_cast<int>(stream.reason().code));
      }
      break;
    }

    ++r.frames;
    rapidjson::Document d;
    d.Parse(frame.c_str());
    if (d.HasParseError() || !d.IsObject() || !d.HasMember("type") ||
        !d["type"].IsString()) {
      continue;
    }
    if (std::string(d["type"].GetString()) != "update") continue;
    ++r.updates;
    if (d.HasMember("value") && d["value"].IsNumber()) {
      const double v = d["value"].GetDouble();
      if (r.updates > 1 && v <= r.lastValue) r.monotonic = false;
      r.lastValue = v;
      if (r.lastValue == target) { r.sawTarget = true; break; }
    }
  }
  return r;
}

std::string readUntilType(ws::stream<tcp::socket>& stream,
                          const char* wantType,
                          std::chrono::milliseconds budget) {
  auto& ioc = static_cast<asio::io_context&>(stream.get_executor().context());
  const auto deadline = std::chrono::steady_clock::now() + budget;
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return {};
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);

    beast::flat_buffer buf;
    std::string frame;
    bool completed = false, ok = false;
    stream.async_read(buf, [&](beast::error_code ec, std::size_t) {
      completed = true;
      if (!ec) { frame = beast::buffers_to_string(buf.data()); ok = true; }
    });
    ioc.restart();
    ioc.run_for(remaining);
    if (!completed) {
      beast::error_code ec;
      stream.next_layer().cancel(ec);
      ioc.restart();
      ioc.run_for(std::chrono::milliseconds(200));
      return {};
    }
    if (!ok) return {};
    rapidjson::Document d;
    d.Parse(frame.c_str());
    if (!d.HasParseError() && d.IsObject() && d.HasMember("type") &&
        d["type"].IsString() &&
        std::string(d["type"].GetString()) == wantType) {
      return frame;
    }
  }
}

double counter(const std::unordered_map<std::string, double>& m, const char* k) {
  auto it = m.find(k);
  return it == m.end() ? 0.0 : it->second;
}

// Subscribe key=1 on (streamKey, "px") and block until the ack lands, so the
// Listener is guaranteed live before the burst starts.
void subscribeAndAwaitAck(ws::stream<tcp::socket>& stream,
                          const std::string& streamKey) {
  const std::string req =
      R"({"type":"subscribe","requests":[{"key":1,"streamKey":")" + streamKey +
      R"(","field":"px"}]})";
  stream.write(asio::buffer(req));
  ASSERT_FALSE(readUntilType(stream, "subscribed", std::chrono::seconds(5)).empty())
      << "subscribe was never acked";
}

// Fire `n` values at (streamKey, "px") straight through the dispatcher, then
// wait for the worker pool to hand every one of them to the session. The
// client is NOT reading while this runs, which is exactly the browser-behind-
// a-replay scenario.
void burst(BurstHarness& srv, const std::string& streamKey, int n) {
  for (int i = 0; i < n; ++i) {
    srv.dispatcher->notifyListeners(streamKey, "px", static_cast<double>(i));
  }
  srv.pool->drain();
}

// Undo the tiny receive buffer before draining. Whatever the server managed to
// push into the kernel socket buffers during the burst still has to be read
// back, and a 2 KB window turns that backlog into minutes of zero-window
// probing — which would trip Beast's 30 s idle timeout and mask the thing under
// test. Widening the window here only affects how fast the *client* catches up;
// the server-side queue behaviour already happened.
void speedUpReader(ws::stream<tcp::socket>& stream) {
  boost::system::error_code ec;
  stream.next_layer().set_option(asio::socket_base::receive_buffer_size(1 << 20), ec);
}

} // namespace

// The documented "safe" side of the old cliff: a burst of exactly
// MAX_OUTBOX_SIZE must never disconnect. Passes on master too — it is the
// baseline the regression test below is measured against.
TEST(ClientSessionBackpressureTest, BurstAtOutboxBoundaryKeepsConnection) {
  constexpr int kTicks = 4096;  // == ClientSession::MAX_OUTBOX_SIZE

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "BOUND");

  burst(srv, "BOUND", kTicks);
  speedUpReader(stream);

  auto r = drainUntilValue(stream, kTicks - 1, std::chrono::seconds(20));
  EXPECT_FALSE(r.closed) << "server killed the session at the boundary: "
                         << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "newest value (" << (kTicks - 1)
                           << ") never arrived; last=" << r.lastValue
                           << " updates=" << r.updates;

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// One past the cliff. On master this is the exact failure the ticket
// describes: `ws.outbox_overflow queueSize=4096` then code=1000
// reason="closing". With flow control the session must survive.
TEST(ClientSessionBackpressureTest, BurstAboveOutboxBoundaryDoesNotDisconnect) {
  constexpr int kTicks = 16384;  // 4x MAX_OUTBOX_SIZE

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "OVER");

  const auto before = gma::util::MetricRegistry::instance().snapshotCounters();
  burst(srv, "OVER", kTicks);
  speedUpReader(stream);

  auto r = drainUntilValue(stream, kTicks - 1, std::chrono::seconds(20));
  const auto after = gma::util::MetricRegistry::instance().snapshotCounters();

  EXPECT_FALSE(r.closed)
      << "a burst past MAX_OUTBOX_SIZE must degrade, not disconnect: "
      << r.closeDetail;
  EXPECT_EQ(counter(after, "ws.outbox_overflow"),
            counter(before, "ws.outbox_overflow"))
      << "the kill-the-consumer path must not fire any more";
  EXPECT_TRUE(r.sawTarget) << "newest value (" << (kTicks - 1)
                           << ") never arrived; last=" << r.lastValue
                           << " updates=" << r.updates;
  // Degradation is the point: a slow reader must not have received every frame.
  EXPECT_LT(r.updates, static_cast<std::size_t>(kTicks))
      << "expected superseded values to be coalesced away";
  EXPECT_GT(counter(after, "ws.outbox_coalesced"),
            counter(before, "ws.outbox_coalesced"))
      << "no frames were coalesced — flow control did not engage";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// The ticket's acceptance criterion: 100k ticks degrade without disconnecting.
TEST(ClientSessionBackpressureTest, HundredThousandTickBurstDegradesWithoutDisconnect) {
  constexpr int kTicks = 100000;

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "HUGE");

  const auto t0 = std::chrono::steady_clock::now();
  burst(srv, "HUGE", kTicks);
  const auto ingestMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

  speedUpReader(stream);
  auto r = drainUntilValue(stream, kTicks - 1, std::chrono::seconds(20));

  RecordProperty("ingest_ms", static_cast<int>(ingestMs));
  RecordProperty("updates_received", static_cast<int>(r.updates));
  std::cerr << "[ENC-996] 100k burst: ingest=" << ingestMs << "ms"
            << " updates_received=" << r.updates
            << " loss=" << (100.0 * (kTicks - static_cast<double>(r.updates)) / kTicks)
            << "%\n";

  EXPECT_FALSE(r.closed) << "100k burst disconnected the client: " << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "newest value never arrived; last=" << r.lastValue
                           << " updates=" << r.updates;
  EXPECT_LT(r.updates, static_cast<std::size_t>(kTicks))
      << "expected lossy degradation, not full delivery";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// Coalescing must apply to value updates only. A control frame queued behind a
// flood (here: the `canceled` ack) is on the lossless path and must still be
// delivered verbatim.
TEST(ClientSessionBackpressureTest, ControlFramesSurviveAnUpdateFlood) {
  constexpr int kTicks = 16384;

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "CTRL");

  burst(srv, "CTRL", kTicks);
  speedUpReader(stream);

  // Queued behind ~16k updates, none of which the client has read yet.
  stream.write(asio::buffer(R"({"type":"cancel","keys":[1]})"));

  EXPECT_FALSE(readUntilType(stream, "canceled", std::chrono::seconds(20)).empty())
      << "the cancel ack was dropped or the session was killed by the flood";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// Coalescing is a backlog policy, not a sampling policy: a burst that never
// pushes the queue past COALESCE_WATERMARK must still arrive complete. Without
// the watermark, coalescing from depth 1 would silently drop intermediates for
// a perfectly healthy consumer — on a fast link there is essentially always one
// write in flight.
TEST(ClientSessionBackpressureTest, BurstBelowWatermarkIsLossless) {
  constexpr int kTicks = 128;  // < ClientSession::COALESCE_WATERMARK (256)

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "SMALL");

  const auto before = gma::util::MetricRegistry::instance().snapshotCounters();
  burst(srv, "SMALL", kTicks);
  speedUpReader(stream);

  auto r = drainUntilValue(stream, kTicks - 1, std::chrono::seconds(20));
  const auto after = gma::util::MetricRegistry::instance().snapshotCounters();

  EXPECT_FALSE(r.closed) << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "last=" << r.lastValue;
  EXPECT_EQ(r.updates, static_cast<std::size_t>(kTicks))
      << "a sub-watermark burst must be lossless";
  EXPECT_EQ(counter(after, "ws.outbox_coalesced"),
            counter(before, "ws.outbox_coalesced"))
      << "coalescing engaged below the watermark";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// Coalescing replaces the NEWEST pending frame for a key, never an older one,
// so surviving values still arrive in the order they were produced. Overwriting
// the oldest pending frame instead would hand the client a new value ahead of
// older ones still queued behind it.
TEST(ClientSessionBackpressureTest, CoalescedValuesStayInOrder) {
  constexpr int kTicks = 16384;

  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());
  subscribeAndAwaitAck(stream, "ORDER");

  burst(srv, "ORDER", kTicks);
  speedUpReader(stream);

  auto r = drainUntilValue(stream, kTicks - 1, std::chrono::seconds(20));
  EXPECT_FALSE(r.closed) << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "last=" << r.lastValue;
  EXPECT_TRUE(r.monotonic)
      << "values went backwards — coalescing overwrote an out-of-order slot";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}
