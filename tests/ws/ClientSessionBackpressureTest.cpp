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
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>

#include <cstdlib>
#include <rapidjson/document.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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
  // Milliseconds between the last update received and the deadline. This is
  // what separates the two failure shapes: a drain that is still delivering
  // when the clock runs out is TOO SLOW, whereas a socket that has been silent
  // for the whole budget is STALLED. Reporting "never arrived" for both is what
  // made ENC-996's CI failure look like a lost value when it was neither.
  long        idleAtDeadlineMs = -1;
};

// How long to wait for the newest value. These tests assert a LIVENESS property
// — that the newest value eventually arrives — not a latency bound, so the
// budget must be generous enough that a slow or contended machine cannot turn
// "still draining" into "dropped it". 20s was not: on a loaded 2-core runner
// the drain of a 100k burst can still be in flight at that point, and the
// resulting failure text ("newest value never arrived") reads as a lost value.
// Passing runs exit as soon as the target arrives, so a large budget costs
// nothing when the property holds. Override with ENC996_BUDGET_MS.
std::chrono::milliseconds drainBudget() {
  if (const char* env = std::getenv("ENC996_BUDGET_MS")) {
    const int ms = std::atoi(env);
    if (ms > 0) return std::chrono::milliseconds(ms);
  }
  return std::chrono::milliseconds(120000);
}

// Formats the distinction above for an assertion message.
std::string drainDiagnosis(const DrainResult& r) {
  if (!r.timedOut) return "drain ended without timing out";
  if (r.idleAtDeadlineMs < 0) return "no updates arrived at all during the budget";
  if (r.idleAtDeadlineMs < 1000)
    return "STILL DRAINING at the deadline (last update " +
           std::to_string(r.idleAtDeadlineMs) +
           "ms before it) — the budget was too short for this machine, not a lost value";
  return "STALLED — silent for " + std::to_string(r.idleAtDeadlineMs) +
         "ms before the deadline";
}

// Read frames until `target` shows up as an update value, the server closes,
// or the budget runs out. Single-threaded (async_read + run_for) so no second
// thread ever touches the socket — same pattern as ClientSessionTest.
DrainResult drainUntilValue(ws::stream<tcp::socket>& stream,
                            double target,
                            std::chrono::milliseconds budget) {
  auto& ioc = static_cast<asio::io_context&>(stream.get_executor().context());
  const auto deadline = std::chrono::steady_clock::now() + budget;
  DrainResult r;
  auto lastUpdateAt = std::chrono::steady_clock::time_point{};

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
    lastUpdateAt = std::chrono::steady_clock::now();
    if (d.HasMember("value") && d["value"].IsNumber()) {
      const double v = d["value"].GetDouble();
      if (r.updates > 1 && v <= r.lastValue) r.monotonic = false;
      r.lastValue = v;
      if (r.lastValue == target) { r.sawTarget = true; break; }
    }
  }
  if (r.timedOut && lastUpdateAt != std::chrono::steady_clock::time_point{}) {
    r.idleAtDeadlineMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - lastUpdateAt).count());
    if (r.idleAtDeadlineMs < 0) r.idleAtDeadlineMs = 0;
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
  // The error_code used to be discarded. If the resize fails — or the kernel
  // silently clamps it — the client keeps reading through the deliberately tiny
  // 2048-byte buffer set by connectSlowReader, the drain runs orders of
  // magnitude slower than the deadline assumes, and the resulting timeout looks
  // exactly like the server having dropped the newest value. That is how
  // ENC-996 read as a flow-control defect when it was a harness one, so the
  // failure is now surfaced here where it is true rather than downstream where
  // it is misleading.
  ASSERT_FALSE(ec) << "speedUpReader: could not widen the receive buffer: "
                   << ec.message()
                   << " — the reader is still throttled, so any drain timeout "
                      "below is a harness artefact, not server behaviour";
  boost::asio::socket_base::receive_buffer_size actual;
  boost::system::error_code getEc;
  stream.next_layer().get_option(actual, getEc);
  if (!getEc && actual.value() <= 4096) {
    ADD_FAILURE() << "speedUpReader: receive buffer is still " << actual.value()
                  << " bytes after asking for " << (1 << 20)
                  << " — the reader did not actually speed up";
  }
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

  auto r = drainUntilValue(stream, kTicks - 1, drainBudget());
  EXPECT_FALSE(r.closed) << "server killed the session at the boundary: "
                         << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "newest value (" << (kTicks - 1)
                           << ") did not arrive; last=" << r.lastValue
                           << " updates=" << r.updates << " — " << drainDiagnosis(r);

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

  auto r = drainUntilValue(stream, kTicks - 1, drainBudget());
  const auto after = gma::util::MetricRegistry::instance().snapshotCounters();

  EXPECT_FALSE(r.closed)
      << "a burst past MAX_OUTBOX_SIZE must degrade, not disconnect: "
      << r.closeDetail;
  EXPECT_EQ(counter(after, "ws.outbox_overflow"),
            counter(before, "ws.outbox_overflow"))
      << "the kill-the-consumer path must not fire any more";
  EXPECT_TRUE(r.sawTarget) << "newest value (" << (kTicks - 1)
                           << ") did not arrive; last=" << r.lastValue
                           << " updates=" << r.updates << " — " << drainDiagnosis(r);
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
  auto r = drainUntilValue(stream, kTicks - 1, drainBudget());

  RecordProperty("ingest_ms", static_cast<int>(ingestMs));
  RecordProperty("updates_received", static_cast<int>(r.updates));
  std::cerr << "[ENC-996] 100k burst: ingest=" << ingestMs << "ms"
            << " updates_received=" << r.updates
            << " loss=" << (100.0 * (kTicks - static_cast<double>(r.updates)) / kTicks)
            << "%\n";

  EXPECT_FALSE(r.closed) << "100k burst disconnected the client: " << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "newest value did not arrive; last=" << r.lastValue
                           << " updates=" << r.updates << " — " << drainDiagnosis(r);
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

  EXPECT_FALSE(readUntilType(stream, "canceled", drainBudget()).empty())
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

  auto r = drainUntilValue(stream, kTicks - 1, drainBudget());
  const auto after = gma::util::MetricRegistry::instance().snapshotCounters();

  EXPECT_FALSE(r.closed) << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "last=" << r.lastValue << " — " << drainDiagnosis(r);
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

  auto r = drainUntilValue(stream, kTicks - 1, drainBudget());
  EXPECT_FALSE(r.closed) << r.closeDetail;
  EXPECT_TRUE(r.sawTarget) << "last=" << r.lastValue << " — " << drainDiagnosis(r);
  EXPECT_TRUE(r.monotonic)
      << "values went backwards — coalescing overwrote an out-of-order slot";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}

// --------------------------------------------------------------------------
// ENC-1072: the hard memory bound must not drop the NEWEST value.
//
// ENC-996 left two drop paths, and only one of them was covered. Below
// MAX_OUTBOX_SIZE a newer value supersedes the pending value for its own key,
// so the newest always survives. AT the bound, `enqueue` used to drop the
// ARRIVING frame — the opposite policy — and no test reached that branch,
// because a burst on a single stream key is pinned near COALESCE_WATERMARK by
// coalescing and never gets within an order of magnitude of 4096.
//
// Reaching the bound therefore needs frames the session may neither coalesce
// nor drop, i.e. LOSSLESS protocol frames, which is exactly how a real session
// gets there: MAX_SUBSCRIPTIONS caps a session at 256 live (subscription,
// streamKey) pairs, so its value frames alone cannot fill 4096 slots — protocol
// traffic queued behind a stalled socket is what pads the queue out to the cap.
// `cancel` is the cheapest generator: every key in the array produces one
// `canceled` ack, with no rate limit and no subscription cap in the way.
//
// The test therefore:
//   1. subscribes 250 padding streams plus one PROBE stream that stays silent,
//   2. seeds one queued value frame per padding stream,
//   3. feeds `canceled` acks in small batches until `ws.outbox_shed` fires,
//      which is the observable proof that outbox_.size() reached
//      MAX_OUTBOX_SIZE (nothing sheds below it),
//   4. and only then produces values on PROBE — a key with nothing pending, so
//      the coalesce path cannot fire and the hard bound must handle it.
//
// On the pre-fix policy every PROBE value is shed and the client never sees a
// single one; the stream reads as permanently dead rather than degraded.
// --------------------------------------------------------------------------

namespace {

// Values for one stream, without the 0-based numbering `burst` hardcodes, so a
// probe stream's values cannot be confused with a padding stream's.
void notifyValue(BurstHarness& srv, const std::string& streamKey, double v) {
  srv.dispatcher->notifyListeners(streamKey, "px", v);
}

double counterNow(const char* name) {
  return counter(gma::util::MetricRegistry::instance().snapshotCounters(), name);
}

// Poll a counter rather than sleeping a fixed amount: these counters are hit on
// the session's strand, so they are the only synchronisation point the test has
// with the server's view of a message it just wrote.
bool waitForCounter(const char* name, double atLeast,
                    std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  for (;;) {
    if (counterNow(name) >= atLeast) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// The padding flood drives thousands of `cancel` messages through the server,
// and each one logs an Info line per key. Left alone that is ~8,500 lines of
// noise per run, which also makes ctest's output capture dominate the test's
// own runtime (7s of gtest becomes 70s of ctest). Quieten the logger for the
// duration and put it back, whatever the test does.
struct QuietLogger {
  gma::util::LogLevel saved;
  QuietLogger() : saved(gma::util::logger().level()) {
    gma::util::logger().setLevel(gma::util::LogLevel::Warn);
  }
  ~QuietLogger() { gma::util::logger().setLevel(saved); }
};

std::string cancelMessage(int firstKey, int count) {
  std::string msg = R"({"type":"cancel","keys":[)";
  for (int i = 0; i < count; ++i) {
    if (i) msg += ',';
    msg += std::to_string(firstKey + i);
  }
  msg += "]}";
  return msg;
}

// Read frames until `target` shows up as an update value ON `wantStream`.
// drainUntilValue matches on the value alone, which is not enough here: the
// padding streams are emitting values of their own throughout.
DrainResult drainUntilStreamValue(ws::stream<tcp::socket>& stream,
                                  const std::string& wantStream,
                                  double target,
                                  std::chrono::milliseconds budget) {
  auto& ioc = static_cast<asio::io_context&>(stream.get_executor().context());
  const auto deadline = std::chrono::steady_clock::now() + budget;
  DrainResult r;
  auto lastUpdateAt = std::chrono::steady_clock::time_point{};

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

    if (!completed) {
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
    if (!d.HasMember("streamKey") || !d["streamKey"].IsString() ||
        wantStream != d["streamKey"].GetString()) {
      continue;
    }
    ++r.updates;
    lastUpdateAt = std::chrono::steady_clock::now();
    if (d.HasMember("value") && d["value"].IsNumber()) {
      r.lastValue = d["value"].GetDouble();
      if (r.lastValue == target) { r.sawTarget = true; break; }
    }
  }
  if (r.timedOut && lastUpdateAt != std::chrono::steady_clock::time_point{}) {
    r.idleAtDeadlineMs = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - lastUpdateAt).count());
    if (r.idleAtDeadlineMs < 0) r.idleAtDeadlineMs = 0;
  }
  return r;
}

} // namespace

TEST(ClientSessionBackpressureTest, NewestValueSurvivesTheHardMemoryBound) {
  constexpr int kPadStreams   = 250;   // < MAX_SUBSCRIPTIONS (256), leaves room for PROBE
  constexpr int kCancelBatch  = 32;    // lossless frames added per round trip
  constexpr int kMaxBatches   = 2000;  // 64,000 frames; ~270 is what it actually takes
  constexpr int kProbeTicks   = 64;
  constexpr double kProbeBase = 1000.0;
  const double kProbeTarget   = kProbeBase + kProbeTicks - 1;
  const std::string kProbe    = "HB_PROBE";

  QuietLogger quiet;
  BurstHarness srv;
  asio::io_context clientIoc;
  auto stream = connectSlowReader(clientIoc, srv.port());

  // ---- 1. One subscribe message: 250 padding streams + the silent probe.
  // One message costs one rate-limit token no matter how many requests it
  // carries, and 251 distinct request keys stays under MAX_SUBSCRIPTIONS.
  std::vector<std::string> padStreams;
  padStreams.reserve(kPadStreams);
  std::string sub = R"({"type":"subscribe","requests":[)";
  for (int i = 0; i < kPadStreams; ++i) {
    char name[16];
    std::snprintf(name, sizeof(name), "HB_PAD%03d", i);
    padStreams.emplace_back(name);
    if (i) sub += ',';
    sub += R"({"key":)" + std::to_string(i + 1) + R"(,"streamKey":")" + padStreams.back() +
           R"(","field":"px"})";
  }
  sub += R"(,{"key":)" + std::to_string(kPadStreams + 1) + R"(,"streamKey":")" + kProbe +
         R"(","field":"px"})";
  sub += "]}";
  stream.write(asio::buffer(sub));

  // Drain every ack before the flood starts, so the queue depth from here on is
  // the test's own doing and not leftover setup traffic.
  for (int i = 0; i <= kPadStreams; ++i) {
    ASSERT_FALSE(readUntilType(stream, "subscribed", std::chrono::seconds(10)).empty())
        << "only " << i << " of " << (kPadStreams + 1) << " subscriptions were acked";
  }

  const auto before = gma::util::MetricRegistry::instance().snapshotCounters();

  // ---- 2. Seed one pending value frame per padding stream. From here the
  // client reads nothing, so the outbox is effectively frozen and everything
  // queued below stays queued.
  for (const auto& s : padStreams) notifyValue(srv, s, 1.0);
  srv.pool->drain();

  // ---- 3. Pad with lossless frames until the hard bound bites.
  // `ws.outbox_shed` cannot fire below MAX_OUTBOX_SIZE — both branches that hit
  // it are inside the `outbox_.size() >= MAX_OUTBOX_SIZE` test — so seeing it
  // move is proof the queue reached the cap.
  const double shedBefore   = counter(before, "ws.outbox_shed");
  double       cancelsSoFar = counter(before, "ws.cancel");
  int          batches      = 0;
  bool         reachedBound = false;

  for (; batches < kMaxBatches && !reachedBound; ++batches) {
    // Keep the padding streams pending: the bound has to have something stale
    // to trade the newest value against.
    for (const auto& s : padStreams) notifyValue(srv, s, 2.0 + batches);
    srv.pool->drain();

    stream.write(asio::buffer(cancelMessage(500000 + batches * kCancelBatch, kCancelBatch)));
    cancelsSoFar += kCancelBatch;
    ASSERT_TRUE(waitForCounter("ws.cancel", cancelsSoFar, std::chrono::seconds(10)))
        << "server stopped processing cancel batches at batch " << batches;

    ASSERT_EQ(counterNow("ws.outbox_overflow"), counter(before, "ws.outbox_overflow"))
        << "the session was killed by the padding flood at batch " << batches
        << " — the queue ran out of shedable frames before the test could probe it";

    reachedBound = counterNow("ws.outbox_shed") > shedBefore;
  }

  ASSERT_TRUE(reachedBound)
      << "never reached MAX_OUTBOX_SIZE after " << batches << " x " << kCancelBatch
      << " lossless frames — the socket is draining faster than the test can fill it, "
         "so the hard-bound policy was never exercised";

  // ---- 4. Now produce on a stream with NOTHING pending. The coalesce path
  // cannot fire for it, so every one of these frames goes through the hard
  // bound.
  const double shedNewestBefore = counterNow("ws.outbox_shed_newest");
  for (int i = 0; i < kProbeTicks; ++i) notifyValue(srv, kProbe, kProbeBase + i);
  srv.pool->drain();

  // ---- 5. The guarantee: the newest value arrives.
  speedUpReader(stream);
  auto r = drainUntilStreamValue(stream, kProbe, kProbeTarget, drainBudget());

  std::cerr << "[ENC-1072] bound reached after " << batches << " cancel batches ("
            << (batches * kCancelBatch) << " lossless frames); probe updates received="
            << r.updates << " last=" << r.lastValue << "\n";

  EXPECT_FALSE(r.closed) << "the session was closed rather than degraded: " << r.closeDetail;
  EXPECT_TRUE(r.sawTarget)
      << "newest value (" << kProbeTarget << ") never arrived; last=" << r.lastValue
      << " updates=" << r.updates << " — " << drainDiagnosis(r)
      << ". At MAX_OUTBOX_SIZE the outbox dropped the ARRIVING value instead of "
         "displacing the oldest queued one, so this stream stays dead for as long "
         "as the client is behind.";
  EXPECT_EQ(counterNow("ws.outbox_shed_newest"), shedNewestBefore)
      << "the outbox gave up on the newest value while it still had stale value "
         "frames it could have displaced";

  beast::error_code ec;
  stream.close(ws::close_code::normal, ec);
}
