// ENC-803 (M17): genuine concurrency / contention tests.
//
// The pre-existing StressTest.cpp writes only DISJOINT (symbol,field) keys —
// fully serialized, zero contention — and AtomicStoreTest's reader/writer test
// ends in SUCCEED() asserting nothing. With the store's locks deleted, both
// still pass. These tests create REAL write-write contention on the SAME key
// from many threads and continuous multi-reader / multi-writer overlap, with
// DETERMINISTIC assertions (value-domain membership, not timing). They are the
// tests that would surface a missing lock under ThreadSanitizer.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gma;

// ---------------------------------------------------------------------------
// Write-write contention on ONE (symbol,field). N threads hammer the same key,
// each thread writing only values drawn from its OWN disjoint band so the final
// survivor identifies exactly one writer. The store must never end up with a
// torn / out-of-domain value; the final value must be the tail of SOME writer.
// ---------------------------------------------------------------------------
TEST(ConcurrencyContentionTest, SameKeyWriteWriteSurvivorIsValid) {
  AtomicStore store;
  constexpr int kThreads = 8;
  constexpr int kWrites  = 5000;

  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; ++t) {
    ths.emplace_back([&store, t]() {
      const int base = t * 1'000'000;
      for (int i = 0; i < kWrites; ++i) {
        store.set("HOT", "px", base + i);
      }
    });
  }
  for (auto& th : ths) th.join();

  auto v = store.get("HOT", "px");
  ASSERT_TRUE(v.has_value());
  int final = std::get<int>(*v);
  // Must be the last value of exactly one writer band: base..base+kWrites-1.
  int band = final / 1'000'000;
  int off  = final % 1'000'000;
  EXPECT_GE(band, 0);
  EXPECT_LT(band, kThreads);
  EXPECT_GE(off, 0);
  EXPECT_LT(off, kWrites) << "torn/garbage value observed: " << final;
}

// ---------------------------------------------------------------------------
// Multi-reader / multi-writer overlap on the SAME key. Writers store only
// values from the closed set [0, kDomain). Readers spin reading the same key
// and flag if they EVER observe a value outside that set (a torn read) or a
// wrong variant alternative. No timing assumptions — the readers loop until the
// writers are done.
// ---------------------------------------------------------------------------
TEST(ConcurrencyContentionTest, MultiReaderMultiWriterNoTornReads) {
  AtomicStore store;
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kDomain  = 256;
  constexpr int kIters   = 20000;

  // Seed so readers always find a value.
  store.set("RW", "v", 0);

  std::atomic<bool> bad{false};
  std::atomic<int>  writersLeft{kWriters};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&store, &writersLeft]() {
      for (int i = 0; i < kIters; ++i) {
        store.set("RW", "v", i % kDomain);
      }
      --writersLeft;
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&store, &bad, &writersLeft]() {
      while (writersLeft.load() > 0) {
        auto v = store.get("RW", "v");
        if (!v) continue;
        try {
          int x = std::get<int>(*v);
          if (x < 0 || x >= kDomain) bad.store(true);
        } catch (...) {
          bad.store(true); // wrong variant alternative => torn write
        }
      }
    });
  }
  for (auto& th : threads) th.join();

  EXPECT_FALSE(bad.load())
      << "reader observed an out-of-domain or torn value — AtomicStore locking "
         "did not hold under multi-reader/multi-writer contention";
}

// ---------------------------------------------------------------------------
// setBatch under concurrent same-symbol writers + readers. Each batch writes a
// coupled pair (a, a*2) for a in [0,kDomain). A reader that observes both keys
// could see two different batches (no cross-key atomicity is promised), but
// each INDIVIDUAL value must be in-domain — i.e. no torn single field.
// ---------------------------------------------------------------------------
TEST(ConcurrencyContentionTest, SetBatchConcurrentSameSymbolInDomain) {
  AtomicStore store;
  constexpr int kWriters = 4;
  constexpr int kDomain  = 512;
  constexpr int kIters   = 10000;

  store.setBatch("B", {{"a", ArgType{0.0}}, {"b", ArgType{0.0}}});

  std::atomic<bool> bad{false};
  std::atomic<int>  writersLeft{kWriters};

  std::vector<std::thread> threads;
  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&store, &writersLeft]() {
      for (int i = 0; i < kIters; ++i) {
        double a = static_cast<double>(i % kDomain);
        store.setBatch("B", {{"a", ArgType{a}}, {"b", ArgType{a * 2.0}}});
      }
      --writersLeft;
    });
  }
  threads.emplace_back([&store, &bad, &writersLeft]() {
    while (writersLeft.load() > 0) {
      auto a = store.get("B", "a");
      auto b = store.get("B", "b");
      try {
        if (a) { double x = std::get<double>(*a); if (x < 0 || x >= kDomain) bad.store(true); }
        if (b) { double y = std::get<double>(*b); if (y < 0 || y >= 2 * kDomain) bad.store(true); }
      } catch (...) { bad.store(true); }
    }
  });
  for (auto& th : threads) th.join();

  EXPECT_FALSE(bad.load()) << "torn setBatch field observed under contention";
}

// ---------------------------------------------------------------------------
// Dispatcher concurrency via its public API: many threads call onTick() on ONE
// shared Dispatcher for the same (symbol,field) while a listener is registered.
// This drives the per-field history, the listener fan-out, and the per-type
// computer cache concurrently. The deterministic contract asserted here is
// only "no crash + the listener saw at least the expected count of in-domain
// values" — enough to be meaningful, and a missing lock surfaces under TSan.
// ---------------------------------------------------------------------------
namespace {
class CountingNode : public INode {
public:
  std::atomic<int> count{0};
  std::atomic<bool> bad{false};
  void onValue(const StreamValue& sv) override {
    try {
      double v = std::get<double>(sv.value);
      // Values are drawn from [0, 1000); anything else is corruption.
      if (v < 0.0 || v >= 1000.0) bad.store(true);
    } catch (...) {
      bad.store(true);
    }
    ++count;
  }
  void shutdown() noexcept override {}
};

Event makeRawTick(const std::string& sym, const char* field, double v) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  doc->AddMember(rapidjson::Value(field, doc->GetAllocator()),
                 rapidjson::Value(v), doc->GetAllocator());
  Event e;
  e.type = "tick";
  e.symbol = sym;
  e.payload = std::move(doc);
  return e;
}
} // namespace

TEST(ConcurrencyContentionTest, DispatcherConcurrentOnTickSameKey) {
  rt::ThreadPool pool(4);
  AtomicStore store;
  util::Config cfg;
  Dispatcher dispatcher(&pool, &store, cfg);

  auto node = std::make_shared<CountingNode>();
  // Bind on a raw, non-price field ("q") so the value is fanned out verbatim
  // by the dispatcher's direct-field path (independent of any TA computer).
  dispatcher.registerListener("CC", "q", node);

  constexpr int kThreads = 6;
  constexpr int kTicks   = 2000;
  std::vector<std::thread> ths;
  for (int t = 0; t < kThreads; ++t) {
    ths.emplace_back([&dispatcher, t]() {
      for (int i = 0; i < kTicks; ++i) {
        dispatcher.onTick(makeRawTick("CC", "q", static_cast<double>((t * 100 + i) % 1000)));
      }
    });
  }
  for (auto& th : ths) th.join();
  pool.drain();
  pool.shutdown();

  EXPECT_FALSE(node->bad.load()) << "listener observed a corrupt value";
  EXPECT_GE(node->count.load(), 1) << "no values delivered under concurrency";
}
