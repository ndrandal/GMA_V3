// tests/ws/SharedSubscriptionE2ETest.cpp — ENC-1041
//
// Cross-connection subscription dedup, driven through TWO real WebSocket
// sessions on one real server. Nothing here is a mock: the only observation
// point that is not the wire is `ExecutionContext::subscriptions()`'s build
// counter, and it is read rather than driven.
//
// WHY `buildCount()` AND NOT A LISTENER COUNT. The question "did two identical
// subscriptions build one pipeline or two?" cannot be answered by counting the
// Dispatcher's listeners: a second identical DAG registers a second Listener on
// the same (streamKey, field), and a second subscriber JOINING registers none —
// but so does a second build that failed, and so does a DAG whose Listener was
// replaced. `buildCount()` is the number of times `tree::buildForRequest` was
// actually run, which is the claim.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/server/WebSocketServer.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace asio  = boost::asio;
namespace ws    = beast::websocket;
using tcp       = asio::ip::tcp;

namespace {

struct Harness {
  asio::io_context ioc;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work;
  std::unique_ptr<gma::rt::ThreadPool>   pool;
  std::unique_ptr<gma::AtomicStore>      store;
  std::unique_ptr<gma::Dispatcher>       dispatcher;
  std::unique_ptr<gma::ExecutionContext> exec;
  std::unique_ptr<gma::WebSocketServer>  server;
  std::thread                            ioThread;

  Harness() {
    gma::registerBuiltinFunctions();
    gma::registerBuiltinNodeTypes();
    pool       = std::make_unique<gma::rt::ThreadPool>(4);
    store      = std::make_unique<gma::AtomicStore>();
    dispatcher = std::make_unique<gma::Dispatcher>(pool.get(), store.get());
    exec       = std::make_unique<gma::ExecutionContext>(store.get(), pool.get());
    server     = std::make_unique<gma::WebSocketServer>(ioc, exec.get(),
                                                        dispatcher.get(), 0);
    server->run();
    work = std::make_unique<
             asio::executor_work_guard<asio::io_context::executor_type>>(
               ioc.get_executor());
    ioThread = std::thread([this] { ioc.run(); });
  }
  ~Harness() {
    try { server->stopAccept(); } catch (...) {}
    try { server->closeAll();   } catch (...) {}
    if (work) work.reset();
    ioc.stop();
    if (ioThread.joinable()) ioThread.join();
  }
  unsigned short port() const { return server->port(); }
  gma::server::SharedSubscriptionRegistry& reg() { return exec->subscriptions(); }
};

// One io_context per client, so each socket is driven by exactly one thread.
struct Client {
  asio::io_context         ioc;
  ws::stream<tcp::socket>  stream;

  explicit Client(unsigned short port) : stream(ioc) {
    tcp::resolver resolver(ioc);
    auto eps = resolver.resolve("127.0.0.1", std::to_string(port));
    asio::connect(stream.next_layer(), eps);
    stream.handshake("127.0.0.1", "/");
  }
  void send(const std::string& s) { stream.write(asio::buffer(s)); }
  void close() {
    beast::error_code ec;
    stream.close(ws::close_code::normal, ec);
  }

  // Bounded single-threaded read loop; returns the first frame whose "type"
  // matches, or "" on timeout. Same shape as ClientSessionTest's reader, and
  // for the same reason: no second thread ever touches this socket.
  std::string readUntilType(const char* wantType,
                            std::chrono::milliseconds budget) {
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
          d["type"].IsString() && std::string(d["type"].GetString()) == wantType)
        return frame;
    }
  }
};

// Poll a predicate so a slow box delays the test rather than making it assert
// a stale value (the ENC-1340 pattern).
template <typename F>
bool waitFor(F&& f, std::chrono::milliseconds budget = std::chrono::seconds(3)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

std::string subInt(int key, const char* streamKey, const char* field,
                   const char* extra = "") {
  return std::string(R"({"type":"subscribe","requests":[{"key":)") +
         std::to_string(key) + R"(,"streamKey":")" + streamKey +
         R"(","field":")" + field + R"(")" + extra + "}]}";
}

std::string subStr(const char* id, const char* streamKey, const char* field,
                   const char* extra = "") {
  return std::string(R"({"type":"subscribe","requests":[{"id":")") + id +
         R"(","streamKey":")" + streamKey + R"(","field":")" + field + R"(")" +
         extra + "}]}";
}

rapidjson::Document parse(const std::string& s) {
  rapidjson::Document d;
  d.Parse(s.c_str());
  return d;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// 1. The headline claim: two connections, identical subscription → ONE
//    computation, both receive every value, and each keeps ITS OWN request key.
//
//    The two connections deliberately use DIFFERENT key types — an int `key`
//    and a string `id` — because that is the harder half of the requirement.
//    One computation now feeds two `Responder`s whose only difference is the
//    key they render, so if the merge had reused one Responder, one client
//    would be reading frames addressed to the other.
// ───────────────────────────────────────────────────────────────────────────
TEST(SharedSubscriptionE2E, TwoConnectionsIdenticalSubscriptionShareOneComputation) {
  Harness srv;
  Client a(srv.port()), b(srv.port());

  a.send(subInt(7, "AAPL", "lastPrice"));
  ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  ASSERT_EQ(srv.reg().buildCount(), 1u);
  ASSERT_EQ(srv.reg().liveKeys(), 1u);

  b.send(subStr("r-AAPL-open", "AAPL", "lastPrice"));
  ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());

  EXPECT_EQ(srv.reg().buildCount(), 1u)
      << "the second identical subscription built a SECOND pipeline — dedup "
         "did not engage, and N browser tabs are N computations";
  EXPECT_EQ(srv.reg().shareCount(), 1u);
  EXPECT_EQ(srv.reg().liveKeys(), 1u) << "two DAGs are registered for one key";
  EXPECT_TRUE(waitFor([&] { return srv.exec->subscriptions().liveKeys() == 1; }));

  // One value, pushed once, must reach both.
  srv.dispatcher->notifyListeners("AAPL", "lastPrice", 101.25);

  const auto fa = a.readUntilType("update", std::chrono::seconds(3));
  const auto fb = b.readUntilType("update", std::chrono::seconds(3));
  ASSERT_FALSE(fa.empty()) << "connection A received no update";
  ASSERT_FALSE(fb.empty()) << "connection B received no update";

  auto da = parse(fa), db = parse(fb);
  ASSERT_FALSE(da.HasParseError());
  ASSERT_FALSE(db.HasParseError());
  EXPECT_DOUBLE_EQ(da["value"].GetDouble(), 101.25);
  EXPECT_DOUBLE_EQ(db["value"].GetDouble(), 101.25);

  // ATTRIBUTION: each frame carries the key ITS subscriber asked under.
  ASSERT_TRUE(da.HasMember("key")) << "A's update lost its int request key";
  EXPECT_EQ(da["key"].GetInt(), 7);
  EXPECT_FALSE(da.HasMember("requestId"))
      << "A's update carried B's string id — one Responder served both";
  ASSERT_TRUE(db.HasMember("requestId")) << "B's update lost its string id";
  EXPECT_STREQ(db["requestId"].GetString(), "r-AAPL-open");
  EXPECT_FALSE(db.HasMember("key"))
      << "B's update carried A's int key — one Responder served both";
}

// ───────────────────────────────────────────────────────────────────────────
// 2. The over-normalisation guard, end to end. Three pairs that must each
//    build TWICE, for three different reasons.
// ───────────────────────────────────────────────────────────────────────────
TEST(SharedSubscriptionE2E, NonIdenticalSubscriptionsDoNotMerge) {
  {   // different field
    Harness srv;
    Client a(srv.port()), b(srv.port());
    a.send(subInt(1, "AAPL", "lastPrice"));
    ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    b.send(subInt(1, "AAPL", "bid"));
    ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    EXPECT_EQ(srv.reg().buildCount(), 2u) << "two different fields merged";
    EXPECT_EQ(srv.reg().liveKeys(), 2u);
    EXPECT_EQ(srv.reg().shareCount(), 0u);
  }
  {   // same field, different streamKey case — AtomicStore is case-sensitive
    Harness srv;
    Client a(srv.port()), b(srv.port());
    a.send(subInt(1, "AAPL", "lastPrice"));
    ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    b.send(subInt(1, "aapl", "lastPrice"));
    ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    EXPECT_EQ(srv.reg().buildCount(), 2u)
        << "'AAPL' and 'aapl' merged — one subscriber is now being fed the "
           "other's stream";
  }
  {   // identical except one carries a `node`
    Harness srv;
    Client a(srv.port()), b(srv.port());
    a.send(subInt(1, "AAPL", "lastPrice"));
    ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    b.send(subInt(1, "AAPL", "lastPrice",
                  R"(,"node":{"type":"AtomicAccessor","streamKey":"AAPL","field":"rsi_14"})"));
    ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
    EXPECT_EQ(srv.reg().buildCount(), 2u)
        << "a bare subscription merged with one carrying a request tree";
  }
}

// An attach-sensitive tree is never shared, so two IDENTICAL such requests
// build twice. That is the refusal doing its job, and it is the thing that
// would silently stop being true if the classification were widened.
TEST(SharedSubscriptionE2E, IdenticalButAttachSensitiveRequestsAreNotShared) {
  Harness srv;
  Client a(srv.port()), b(srv.port());
  const char* worker = R"(,"pipeline":[{"type":"Worker","fn":"mean"}])";

  a.send(subInt(1, "AAPL", "lastPrice", worker));
  ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  b.send(subInt(2, "AAPL", "lastPrice", worker));
  ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());

  EXPECT_EQ(srv.reg().buildCount(), 0u)
      << "a request whose tree contains a `Worker` was SHARED. Worker::acc_ "
         "retains up to 1000 values per symbol and reduces over all of them "
         "with no clear, so the late subscriber would receive a value derived "
         "from history it was not present for.";
  EXPECT_EQ(srv.reg().liveKeys(), 0u);

  // Both still work — a refusal to share is a refusal to share, not a refusal
  // to serve.
  srv.dispatcher->notifyListeners("AAPL", "lastPrice", 50.0);
  EXPECT_FALSE(a.readUntilType("update", std::chrono::seconds(3)).empty());
  EXPECT_FALSE(b.readUntilType("update", std::chrono::seconds(3)).empty());
}

// ───────────────────────────────────────────────────────────────────────────
// 3. Lifetime: a disconnect by one of N must not disturb the others, and the
//    last one out tears the computation down.
// ───────────────────────────────────────────────────────────────────────────
TEST(SharedSubscriptionE2E, OneDisconnectLeavesTheOthersReceiving) {
  Harness srv;
  auto a = std::make_unique<Client>(srv.port());
  Client b(srv.port()), c(srv.port());

  a->send(subInt(1, "MSFT", "lastPrice"));
  ASSERT_FALSE(a->readUntilType("subscribed", std::chrono::seconds(3)).empty());
  b.send(subInt(2, "MSFT", "lastPrice"));
  ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  c.send(subStr("c", "MSFT", "lastPrice"));
  ASSERT_FALSE(c.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  ASSERT_EQ(srv.reg().buildCount(), 1u);
  ASSERT_TRUE(waitFor([&] { return srv.reg().liveKeys() == 1; }));

  // A leaves.
  a->close();
  a.reset();
  ASSERT_TRUE(waitFor([&] {
    // Exactly one subscriber gone, DAG intact.
    return srv.reg().liveKeys() == 1;
  })) << "the DAG was torn down when the FIRST of three subscribers left";

  srv.dispatcher->notifyListeners("MSFT", "lastPrice", 311.5);
  const auto fb = b.readUntilType("update", std::chrono::seconds(3));
  const auto fc = c.readUntilType("update", std::chrono::seconds(3));
  EXPECT_FALSE(fb.empty())
      << "B stopped receiving when an unrelated connection disconnected";
  EXPECT_FALSE(fc.empty())
      << "C stopped receiving when an unrelated connection disconnected";
  if (!fb.empty()) EXPECT_DOUBLE_EQ(parse(fb)["value"].GetDouble(), 311.5);
  if (!fc.empty()) EXPECT_DOUBLE_EQ(parse(fc)["value"].GetDouble(), 311.5);
  EXPECT_EQ(srv.reg().buildCount(), 1u)
      << "the DAG was rebuilt after a disconnect";

  // B cancels explicitly, C disconnects: the last one out tears it down.
  b.send(R"({"type":"cancel","keys":[2]})");
  ASSERT_FALSE(b.readUntilType("canceled", std::chrono::seconds(3)).empty());
  EXPECT_TRUE(waitFor([&] { return srv.reg().liveKeys() == 1; }))
      << "the DAG went away while C was still subscribed";

  c.close();
  EXPECT_TRUE(waitFor([&] { return srv.reg().liveKeys() == 0; }))
      << "the LAST subscriber left and the shared DAG was not torn down — "
         "every listener it registered is leaked for the life of the server";
  EXPECT_EQ(srv.reg().buildCount(), 1u);
}

// A cancel by one of two is the same claim on the protocol path rather than the
// socket-teardown path, and it is the one `handleCancel` owns.
TEST(SharedSubscriptionE2E, CancelByOneOfTwoLeavesTheOtherReceiving) {
  Harness srv;
  Client a(srv.port()), b(srv.port());
  a.send(subInt(11, "GOOG", "lastPrice"));
  ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  b.send(subInt(12, "GOOG", "lastPrice"));
  ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  ASSERT_EQ(srv.reg().buildCount(), 1u);

  a.send(R"({"type":"cancel","keys":[11]})");
  ASSERT_FALSE(a.readUntilType("canceled", std::chrono::seconds(3)).empty());
  ASSERT_TRUE(waitFor([&] { return srv.reg().liveKeys() == 1; }));

  srv.dispatcher->notifyListeners("GOOG", "lastPrice", 142.0);
  const auto fb = b.readUntilType("update", std::chrono::seconds(3));
  ASSERT_FALSE(fb.empty())
      << "a cancel on one connection stopped another connection's values";
  EXPECT_DOUBLE_EQ(parse(fb)["value"].GetDouble(), 142.0);
  EXPECT_EQ(parse(fb)["key"].GetInt(), 12);
}

// ───────────────────────────────────────────────────────────────────────────
// 4. ATTRIBUTION (ENC-1396) on a would-be-merged key. Both connections send a
//    request that is SHAREABLE by the equivalence relation but FAILS TO BUILD —
//    a Listener bound to a pipeline-only `ob.*` field. Each must be rejected
//    with its own request key in the error frame; neither may inherit the
//    other's silence, and the failure must not be cached.
// ───────────────────────────────────────────────────────────────────────────
TEST(SharedSubscriptionE2E, ABuildRejectionIsAttributedToEachRequestKey) {
  Harness srv;
  Client a(srv.port()), b(srv.port());

  a.send(subInt(31, "AAPL", "ob.spread"));
  const auto ea = a.readUntilType("error", std::chrono::seconds(3));
  ASSERT_FALSE(ea.empty()) << "A's unbuildable request produced no error frame";
  auto da = parse(ea);
  ASSERT_FALSE(da.HasParseError());
  EXPECT_STREQ(da["where"].GetString(), "build");
  ASSERT_TRUE(da.HasMember("key"))
      << "ENC-1396 regression: the error names no request. frame=" << ea;
  EXPECT_EQ(da["key"].GetInt(), 31);

  // Same canonical key, different subscriber. If the failed build had been
  // cached, B would get either no diagnostic or A's.
  b.send(subStr("r-ob", "AAPL", "ob.spread"));
  const auto eb = b.readUntilType("error", std::chrono::seconds(3));
  ASSERT_FALSE(eb.empty())
      << "B's identical unbuildable request produced no error frame — the "
         "failed build was cached and B inherited A's silence";
  auto db = parse(eb);
  ASSERT_FALSE(db.HasParseError());
  EXPECT_STREQ(db["where"].GetString(), "build");
  ASSERT_TRUE(db.HasMember("requestId"))
      << "B's rejection was not attributed to B's request. frame=" << eb;
  EXPECT_STREQ(db["requestId"].GetString(), "r-ob");
  EXPECT_FALSE(db.HasMember("key")) << "B's rejection carried A's int key";

  EXPECT_EQ(srv.reg().liveKeys(), 0u)
      << "a failed build left a registry entry behind, so the next identical "
         "request would attach to a DAG that does not exist";
}

// A per-request reject that fires BEFORE the dedup lookup must keep its
// attribution too — dedup sits after every one of them, and this is the
// cheapest way to say so in a test rather than only in a comment.
TEST(SharedSubscriptionE2E, PreDedupRejectsKeepTheirRequestKey) {
  Harness srv;
  Client a(srv.port());
  a.send(R"({"type":"subscribe","requests":[{"key":44,"streamKey":"AAPL"}]}
)");
  const auto e = a.readUntilType("error", std::chrono::seconds(3));
  ASSERT_FALSE(e.empty());
  auto d = parse(e);
  ASSERT_FALSE(d.HasParseError());
  EXPECT_STREQ(d["where"].GetString(), "subscribe");
  ASSERT_TRUE(d.HasMember("key"));
  EXPECT_EQ(d["key"].GetInt(), 44);
  EXPECT_EQ(srv.reg().buildCount(), 0u)
      << "a request rejected before the build reached the registry";
}

// ───────────────────────────────────────────────────────────────────────────
// 5. THE CORRECTNESS CLAIM, not the resource claim: a merged subscription
//    delivers exactly what an unmerged one would.
//
//    This is the question "is dedup a correctness change?" asked directly. A
//    LATE subscriber joins a DAG that has already been running and carrying
//    values. What it must receive is exactly what its own freshly-built DAG
//    would have delivered from that moment: every subsequent value, in order,
//    and NOTHING from before it attached.
//
//    "Nothing from before" is the half that could have gone wrong and is the
//    reason the sharing rule refuses an attach-sensitive tree. For the
//    shareable class it holds structurally — `Dispatcher::registerListener`
//    appends without replaying a last value, and no node on a shareable DAG
//    accumulates across values — and this test is where that is observed
//    rather than argued.
// ───────────────────────────────────────────────────────────────────────────
TEST(SharedSubscriptionE2E, ALateJoinerReceivesExactlyWhatAFreshDagWould) {
  Harness srv;
  Client a(srv.port());

  a.send(subInt(1, "NVDA", "lastPrice"));
  ASSERT_FALSE(a.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  ASSERT_EQ(srv.reg().buildCount(), 1u);

  // Three values BEFORE the second subscriber exists.
  const double before[] = {10.0, 11.0, 12.0};
  for (double v : before) {
    srv.dispatcher->notifyListeners("NVDA", "lastPrice", v);
    const auto f = a.readUntilType("update", std::chrono::seconds(3));
    ASSERT_FALSE(f.empty()) << "A missed pre-join value " << v;
    EXPECT_DOUBLE_EQ(parse(f)["value"].GetDouble(), v);
  }

  // B joins the already-running DAG.
  Client b(srv.port());
  b.send(subStr("late", "NVDA", "lastPrice"));
  ASSERT_FALSE(b.readUntilType("subscribed", std::chrono::seconds(3)).empty());
  EXPECT_EQ(srv.reg().buildCount(), 1u) << "B built its own DAG";
  EXPECT_EQ(srv.reg().shareCount(), 1u);

  // Two more values. Both subscribers must see the SAME sequence, in order.
  const double after[] = {13.0, 14.0};
  for (double v : after) {
    srv.dispatcher->notifyListeners("NVDA", "lastPrice", v);

    const auto fa = a.readUntilType("update", std::chrono::seconds(3));
    ASSERT_FALSE(fa.empty()) << "A missed post-join value " << v;
    auto da = parse(fa);
    EXPECT_DOUBLE_EQ(da["value"].GetDouble(), v);
    EXPECT_EQ(da["key"].GetInt(), 1);

    const auto fb = b.readUntilType("update", std::chrono::seconds(3));
    ASSERT_FALSE(fb.empty()) << "B missed post-join value " << v;
    auto db = parse(fb);
    // The decisive assertion: B's FIRST frame is 13.0, not 10.0. A replay of
    // the DAG's accumulated state — or any node holding a pre-join value —
    // would surface here as B being handed a number it was never present for.
    EXPECT_DOUBLE_EQ(db["value"].GetDouble(), v)
        << "the late joiner received a value from before it attached, or the "
           "sequences diverged. A merged subscription must deliver exactly "
           "what an unmerged one would.";
    EXPECT_STREQ(db["requestId"].GetString(), "late");
    // Same value, same bucket identity — the two frames differ only in the
    // request key they are addressed to.
    EXPECT_EQ(da.HasMember("bucketStartMs"), db.HasMember("bucketStartMs"));
    if (da.HasMember("bucketStartMs") && db.HasMember("bucketStartMs")) {
      EXPECT_EQ(da["bucketStartMs"].GetInt64(), db["bucketStartMs"].GetInt64());
    }
  }

  // And B never had a stale frame waiting: with both streams drained above,
  // there is nothing further queued for it.
  EXPECT_TRUE(b.readUntilType("update", std::chrono::milliseconds(300)).empty())
      << "the late joiner had an extra update queued — it received more values "
         "than its own DAG would have produced";
}
