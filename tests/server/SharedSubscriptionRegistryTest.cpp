// tests/server/SharedSubscriptionRegistryTest.cpp — ENC-1041
//
// Unit coverage for the two mechanisms behind cross-connection dedup, below the
// WebSocket layer: `nodes::SharedTerminal` (dynamic fan-out, per-sink failure
// isolation) and `server::SharedSubscriptionRegistry` (build-once, refcount,
// teardown-on-last, and NO caching of a failed build).
//
// The end-to-end claims — two real sockets, two request keys, one computation —
// are in tests/ws/SharedSubscriptionE2ETest.cpp. These are the invariants that
// an end-to-end test can only observe indirectly.

#include "gma/nodes/SharedTerminal.hpp"
#include "gma/server/SharedSubscriptionRegistry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using gma::INode;
using gma::StreamValue;
using gma::nodes::SharedTerminal;
using gma::server::SharedSubscriptionRegistry;

namespace {

// Counts values and shutdowns. `throwOnValue` makes it the badly-behaved sink.
struct CountingSink : INode {
  std::atomic<int>  values{0};
  std::atomic<int>  shutdowns{0};
  std::atomic<bool> throwOnValue{false};
  std::atomic<double> last{0.0};

  void onValue(const StreamValue& sv) override {
    ++values;
    if (std::holds_alternative<double>(sv.value))
      last.store(std::get<double>(sv.value));
    if (throwOnValue.load()) throw std::runtime_error("sink exploded");
  }
  void shutdown() noexcept override { ++shutdowns; }
};

StreamValue val(double d) { return StreamValue{"AAPL", d, 0}; }

// A head node, so the registry has something to tear down on last release.
struct RecordingHead : INode {
  std::atomic<int> shutdowns{0};
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override { ++shutdowns; }
};

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// SharedTerminal
// ───────────────────────────────────────────────────────────────────────────

TEST(SharedTerminal, FansOneValueToEverySink) {
  auto t  = std::make_shared<SharedTerminal>();
  auto s1 = std::make_shared<CountingSink>();
  auto s2 = std::make_shared<CountingSink>();
  ASSERT_NE(t->attach(s1), 0u);
  ASSERT_NE(t->attach(s2), 0u);
  EXPECT_EQ(t->size(), 2u);

  t->onValue(val(1.5));
  EXPECT_EQ(s1->values.load(), 1);
  EXPECT_EQ(s2->values.load(), 1);
  EXPECT_DOUBLE_EQ(s1->last.load(), 1.5);
  EXPECT_DOUBLE_EQ(s2->last.load(), 1.5);
}

// The disconnect claim at the node level: removing one sink leaves the others
// receiving, and shuts down ONLY the removed one — via the caller, because
// detach deliberately hands the sink back rather than stopping it under a lock.
TEST(SharedTerminal, DetachingOneLeavesTheOthersReceiving) {
  auto t  = std::make_shared<SharedTerminal>();
  auto s1 = std::make_shared<CountingSink>();
  auto s2 = std::make_shared<CountingSink>();
  const auto id1 = t->attach(s1);
  t->attach(s2);

  auto det = t->detach(id1);
  ASSERT_TRUE(det.found);
  EXPECT_EQ(det.sink, s1);
  EXPECT_EQ(det.remaining, 1u);
  EXPECT_EQ(s1->shutdowns.load(), 0)
      << "detach shut the sink down itself — it must hand it back so the "
         "caller can do that with no lock held";

  t->onValue(val(2.0));
  EXPECT_EQ(s1->values.load(), 0) << "a detached sink still received a value";
  EXPECT_EQ(s2->values.load(), 1) << "the surviving sink stopped receiving";
}

// A failure mode created purely by sharing: before dedup, a throwing sink cost
// one client its own values. Across connections it would cost every LATER sink
// in the fan-out loop theirs.
TEST(SharedTerminal, OneSinkThrowingDoesNotStarveTheOthers) {
  auto t    = std::make_shared<SharedTerminal>();
  auto bad  = std::make_shared<CountingSink>();
  auto good = std::make_shared<CountingSink>();
  bad->throwOnValue.store(true);
  t->attach(bad);
  t->attach(good);   // attached AFTER the thrower, so it is the one at risk

  EXPECT_NO_THROW(t->onValue(val(3.0)));
  EXPECT_EQ(bad->values.load(), 1);
  EXPECT_EQ(good->values.load(), 1)
      << "the throwing sink aborted the fan-out and the next subscriber lost "
         "the value";
}

TEST(SharedTerminal, SubscriberIdsAreNeverReused) {
  auto t  = std::make_shared<SharedTerminal>();
  auto s1 = std::make_shared<CountingSink>();
  auto s2 = std::make_shared<CountingSink>();
  const auto id1 = t->attach(s1);
  t->detach(id1);
  const auto id2 = t->attach(s2);
  EXPECT_NE(id1, id2)
      << "a reused id lets a stale detach remove somebody else's sink";
  // And a stale id is inert.
  auto det = t->detach(id1);
  EXPECT_FALSE(det.found);
  EXPECT_EQ(t->size(), 1u);
}

TEST(SharedTerminal, ShutdownStopsEverySinkAndRefusesNewOnes) {
  auto t  = std::make_shared<SharedTerminal>();
  auto s1 = std::make_shared<CountingSink>();
  t->attach(s1);
  t->shutdown();
  EXPECT_EQ(s1->shutdowns.load(), 1);
  EXPECT_EQ(t->size(), 0u);

  auto s2 = std::make_shared<CountingSink>();
  EXPECT_EQ(t->attach(s2), 0u)
      << "attaching to a dead terminal registers a subscriber that can never "
         "receive a value — the caller must be told so it can build its own";
  t->onValue(val(4.0));
  EXPECT_EQ(s1->values.load(), 0);
  EXPECT_EQ(s2->values.load(), 0);
}

// ───────────────────────────────────────────────────────────────────────────
// SharedSubscriptionRegistry
// ───────────────────────────────────────────────────────────────────────────

namespace {

// A builder that counts invocations and hangs the given head off the DAG.
SharedSubscriptionRegistry::Builder
makeBuilder(std::atomic<int>& calls,
            std::shared_ptr<RecordingHead> head = nullptr) {
  return [&calls, head](std::shared_ptr<SharedTerminal>)
             -> SharedSubscriptionRegistry::Built {
    ++calls;
    SharedSubscriptionRegistry::Built b;
    b.head = head ? head : std::make_shared<RecordingHead>();
    return b;
  };
}

} // namespace

// The headline claim, at the registry level: identical keys build ONCE.
// `buildCount()` exists because no count of live keys or of subscribers can
// tell "built once and joined" from "built twice and one replaced the other".
TEST(SharedSubscriptionRegistry, IdenticalKeysBuildExactlyOnce) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto s1 = std::make_shared<CountingSink>();
  auto s2 = std::make_shared<CountingSink>();

  auto l1 = reg.acquire("K", s1, makeBuilder(calls));
  auto l2 = reg.acquire("K", s2, makeBuilder(calls));
  ASSERT_TRUE(l1.valid());
  ASSERT_TRUE(l2.valid());

  EXPECT_EQ(calls.load(), 1)      << "the builder ran twice for one key";
  EXPECT_EQ(reg.buildCount(), 1u);
  EXPECT_EQ(reg.shareCount(), 1u);
  EXPECT_EQ(reg.liveKeys(), 1u);
  EXPECT_EQ(reg.refCount("K"), 2u);
  EXPECT_NE(l1.subscriberId(), l2.subscriberId());
}

TEST(SharedSubscriptionRegistry, DifferentKeysBuildSeparately) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto l1 = reg.acquire("A", std::make_shared<CountingSink>(), makeBuilder(calls));
  auto l2 = reg.acquire("B", std::make_shared<CountingSink>(), makeBuilder(calls));
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(reg.liveKeys(), 2u);
  EXPECT_EQ(reg.shareCount(), 0u);
}

// The disconnect claim: one of N leaving must not tear down the computation.
TEST(SharedSubscriptionRegistry, FirstOfTwoReleasesDoesNotTearDownTheDag) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto head = std::make_shared<RecordingHead>();
  auto s1   = std::make_shared<CountingSink>();
  auto s2   = std::make_shared<CountingSink>();

  auto l1 = reg.acquire("K", s1, makeBuilder(calls, head));
  auto l2 = reg.acquire("K", s2, makeBuilder(calls, head));
  l1.release();

  EXPECT_FALSE(l1.valid());
  EXPECT_EQ(head->shutdowns.load(), 0)
      << "the shared DAG was shut down by the FIRST of two subscribers leaving";
  EXPECT_EQ(s1->shutdowns.load(), 1) << "the leaver's own sink was not stopped";
  EXPECT_EQ(s2->shutdowns.load(), 0) << "a surviving subscriber's sink was stopped";
  EXPECT_EQ(reg.liveKeys(), 1u);
  EXPECT_EQ(reg.refCount("K"), 1u);

  l2.release();
  EXPECT_EQ(head->shutdowns.load(), 1) << "the last release did not tear down";
  EXPECT_EQ(s2->shutdowns.load(), 1);
  EXPECT_EQ(reg.liveKeys(), 0u);
  EXPECT_EQ(reg.refCount("K"), 0u);
}

TEST(SharedSubscriptionRegistry, ALeaseReleasesOnDestructionAndIsIdempotent) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto head = std::make_shared<RecordingHead>();
  {
    auto l = reg.acquire("K", std::make_shared<CountingSink>(),
                         makeBuilder(calls, head));
    ASSERT_TRUE(l.valid());
    l.release();
    l.release();                       // second call must be inert
    EXPECT_EQ(head->shutdowns.load(), 1);
  }
  EXPECT_EQ(head->shutdowns.load(), 1) << "destruction re-ran the release";
  EXPECT_EQ(reg.liveKeys(), 0u);

  // And a key torn down is rebuilt, not resurrected.
  auto l2 = reg.acquire("K", std::make_shared<CountingSink>(), makeBuilder(calls));
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(reg.buildCount(), 2u);
}

TEST(SharedSubscriptionRegistry, AMovedLeaseDoesNotDoubleRelease) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto head = std::make_shared<RecordingHead>();
  auto l1 = reg.acquire("K", std::make_shared<CountingSink>(),
                        makeBuilder(calls, head));
  auto l2 = std::move(l1);
  EXPECT_FALSE(l1.valid());
  ASSERT_TRUE(l2.valid());
  EXPECT_EQ(reg.refCount("K"), 1u);
  l2.release();
  EXPECT_EQ(head->shutdowns.load(), 1);
  EXPECT_EQ(reg.liveKeys(), 0u);
}

// ATTRIBUTION (ENC-1396). A build that throws must propagate UNCHANGED and
// leave nothing behind, so `handleSubscribe`'s catch reports it under the
// request key that caused it — and a later identical request re-runs the build
// and is rejected under ITS key rather than inheriting a cached silence.
TEST(SharedSubscriptionRegistry, AFailedBuildIsNotCachedAndRethrowsUnchanged) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto failing = [&calls](std::shared_ptr<SharedTerminal>)
                     -> SharedSubscriptionRegistry::Built {
    ++calls;
    throw std::runtime_error("listener: field 'ob.spread' is pipeline-only");
  };

  auto s1 = std::make_shared<CountingSink>();
  try {
    reg.acquire("K", s1, failing);
    FAIL() << "acquire swallowed the builder's exception";
  } catch (const std::runtime_error& ex) {
    EXPECT_STREQ(ex.what(), "listener: field 'ob.spread' is pipeline-only")
        << "the builder's message was rewritten on the way out — the client "
           "would be told something other than why its request failed";
  }
  EXPECT_EQ(reg.liveKeys(), 0u) << "a failed build left an entry behind";
  EXPECT_EQ(reg.refCount("K"), 0u);
  EXPECT_EQ(s1->shutdowns.load(), 0);

  // Second, identical request: the build runs AGAIN. If a failure were cached,
  // this subscriber would either be refused without its own diagnostic or be
  // attached to nothing.
  auto s2 = std::make_shared<CountingSink>();
  EXPECT_THROW(reg.acquire("K", s2, failing), std::runtime_error);
  EXPECT_EQ(calls.load(), 2)
      << "the second identical request did not re-run the build, so its "
         "rejection could not be attributed to its own request key";
}

// The concurrency claim. N threads racing on one missing key must produce ONE
// DAG — the `Building` placeholder plus the condition variable, not a lucky
// interleaving.
TEST(SharedSubscriptionRegistry, ConcurrentAcquireOfOneKeyBuildsOnce) {
  constexpr int kThreads = 16;
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  std::atomic<int> valid{0};
  auto head = std::make_shared<RecordingHead>();

  std::barrier sync(kThreads);
  std::vector<SharedSubscriptionRegistry::Lease> leases(kThreads);
  std::vector<std::thread> ts;
  for (int i = 0; i < kThreads; ++i) {
    ts.emplace_back([&, i] {
      auto builder = [&](std::shared_ptr<SharedTerminal>)
                         -> SharedSubscriptionRegistry::Built {
        ++calls;
        // Hold the build open so the other threads are guaranteed to arrive
        // while this key is in its `Building` state — the window the
        // condition variable exists for.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SharedSubscriptionRegistry::Built b;
        b.head = head;
        return b;
      };
      sync.arrive_and_wait();
      leases[static_cast<std::size_t>(i)] =
          reg.acquire("K", std::make_shared<CountingSink>(), builder);
      if (leases[static_cast<std::size_t>(i)].valid()) ++valid;
    });
  }
  for (auto& t : ts) t.join();

  EXPECT_EQ(calls.load(), 1)
      << kThreads << " threads raced on one key and built " << calls.load()
      << " DAGs — the Building placeholder did not hold";
  EXPECT_EQ(reg.buildCount(), 1u);
  EXPECT_EQ(valid.load(), kThreads);
  EXPECT_EQ(reg.refCount("K"), static_cast<std::size_t>(kThreads));
  EXPECT_EQ(head->shutdowns.load(), 0);

  // Releasing all but one must not tear down; the last must.
  for (int i = 0; i < kThreads - 1; ++i) leases[static_cast<std::size_t>(i)].release();
  EXPECT_EQ(head->shutdowns.load(), 0);
  EXPECT_EQ(reg.refCount("K"), 1u);
  leases[kThreads - 1].release();
  EXPECT_EQ(head->shutdowns.load(), 1);
  EXPECT_EQ(reg.liveKeys(), 0u);
}

TEST(SharedSubscriptionRegistry, ShutdownAllStopsEveryDagAndLeasesGoInert) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  auto hA = std::make_shared<RecordingHead>();
  auto hB = std::make_shared<RecordingHead>();
  auto sA = std::make_shared<CountingSink>();
  auto lA = reg.acquire("A", sA, makeBuilder(calls, hA));
  auto lB = reg.acquire("B", std::make_shared<CountingSink>(),
                        makeBuilder(calls, hB));

  reg.shutdownAll();
  EXPECT_EQ(hA->shutdowns.load(), 1);
  EXPECT_EQ(hB->shutdowns.load(), 1);
  EXPECT_EQ(sA->shutdowns.load(), 1) << "shutdownAll did not stop the sinks";
  EXPECT_EQ(reg.liveKeys(), 0u);

  // The outstanding leases must not double-shutdown or crash.
  EXPECT_NO_THROW(lA.release());
  EXPECT_NO_THROW(lB.release());
  EXPECT_EQ(hA->shutdowns.load(), 1);
  EXPECT_NO_THROW(reg.shutdownAll());   // idempotent
}

TEST(SharedSubscriptionRegistry, ANullSinkOrBuilderYieldsNoLeaseAndNoBuild) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  EXPECT_FALSE(reg.acquire("K", nullptr, makeBuilder(calls)).valid());
  EXPECT_FALSE(reg.acquire("K", std::make_shared<CountingSink>(),
                           SharedSubscriptionRegistry::Builder{}).valid());
  EXPECT_EQ(calls.load(), 0);
  EXPECT_EQ(reg.liveKeys(), 0u);
}

// A shared DAG fans ONE value to every attached sink. This is the "O(1) in N"
// claim stated as a count rather than as a timing: the builder ran once, the
// head is one object, and one value emitted at the terminal reaches N sinks.
TEST(SharedSubscriptionRegistry, OneValueReachesEverySubscriberOfOneDag) {
  SharedSubscriptionRegistry reg;
  std::atomic<int> calls{0};
  std::shared_ptr<SharedTerminal> captured;
  auto builder = [&](std::shared_ptr<SharedTerminal> t)
                     -> SharedSubscriptionRegistry::Built {
    ++calls;
    captured = std::move(t);
    SharedSubscriptionRegistry::Built b;
    b.head = std::make_shared<RecordingHead>();
    return b;
  };

  constexpr int kN = 8;
  std::vector<std::shared_ptr<CountingSink>> sinks;
  std::vector<SharedSubscriptionRegistry::Lease> leases;
  for (int i = 0; i < kN; ++i) {
    sinks.push_back(std::make_shared<CountingSink>());
    leases.push_back(reg.acquire("K", sinks.back(), builder));
    ASSERT_TRUE(leases.back().valid()) << "subscriber " << i;
  }
  ASSERT_TRUE(captured);
  EXPECT_EQ(calls.load(), 1)
      << kN << " subscribers caused " << calls.load() << " builds";
  EXPECT_EQ(captured->size(), static_cast<std::size_t>(kN));

  captured->onValue(val(7.25));
  for (int i = 0; i < kN; ++i) {
    EXPECT_EQ(sinks[static_cast<std::size_t>(i)]->values.load(), 1)
        << "subscriber " << i << " did not receive the shared value";
    EXPECT_DOUBLE_EQ(sinks[static_cast<std::size_t>(i)]->last.load(), 7.25);
  }
}
