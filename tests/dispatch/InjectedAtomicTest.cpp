// ENC-1007 — injected values must reach AtomicStore, not just listeners.
//
// §8.1 wants "on an interval, grab that atomic value". Before ENC-1007 the
// Dispatcher wrote only DERIVED atomics (FunctionMap builtins keyed by bare
// function name) into the AtomicStore; the injected RAW value was handed to
// Listeners and then dropped. AtomicAccessor reads the store, so an externally
// injected series — a client bringing its own RSI rather than having gma
// recompute it — was unreachable from an Interval-driven tree.
//
// The pre-ENC-1007 behaviour is pinned negatively too (see the control cases):
// a field that was never injected must still resolve to nothing, so these
// tests fail on real data flow rather than on an echo.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/Interval.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gma;

namespace {

class Recorder : public INode {
public:
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    received_.push_back(sv);
  }
  void shutdown() noexcept override {}

  std::vector<StreamValue> snapshot() const {
    std::lock_guard<std::mutex> lk(mx_);
    return received_;
  }
  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mx_);
    return received_.size();
  }

private:
  mutable std::mutex mx_;
  std::vector<StreamValue> received_;
};

// A tick carrying arbitrary externally-computed fields — the injection shape
// §8.1 describes (unknown symbol, unknown field names).
Event makeTick(const std::string& symbol,
               const std::vector<std::pair<std::string, double>>& fields) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  auto& alloc = doc->GetAllocator();
  for (const auto& [name, value] : fields) {
    doc->AddMember(rapidjson::Value(name.c_str(), alloc),
                   rapidjson::Value(value), alloc);
  }
  return Event{symbol, std::move(doc)};
}

double asDouble(const ArgType& v) {
  if (std::holds_alternative<double>(v)) return std::get<double>(v);
  if (std::holds_alternative<int>(v)) return static_cast<double>(std::get<int>(v));
  ADD_FAILURE() << "stored value is neither double nor int";
  return 0.0;
}

}  // namespace

// --- The AC, first half: readable via AtomicAccessor. ----------------------

TEST(InjectedAtomicTest, InjectedValueIsReadableViaAtomicAccessor) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 71.5}}));

  auto downstream = std::make_shared<Recorder>();
  AtomicAccessor accessor("EXT.SYM", "my_external_rsi", &store, downstream);
  accessor.onValue(StreamValue{"", 0.0});

  pool.shutdown();

  ASSERT_EQ(downstream->size(), 1u)
      << "injected value never reached AtomicStore, so AtomicAccessor read nothing";
  EXPECT_DOUBLE_EQ(asDouble(downstream->snapshot()[0].value), 71.5);
}

// No Listener is registered anywhere in these tests on purpose: an
// Interval-driven AtomicAccessor is a PULL consumer, so requiring a
// registered Listener to make the value materialise would leave §8.1
// unachievable in exactly the configuration it describes.
TEST(InjectedAtomicTest, InjectionDoesNotRequireARegisteredListener) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 12.25}}));
  pool.shutdown();

  auto stored = store.get("EXT.SYM", "my_external_rsi");
  ASSERT_TRUE(stored.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*stored), 12.25);
}

TEST(InjectedAtomicTest, LatestInjectedValueWins) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  for (double v : {10.0, 15.0, 20.0, 25.0, 30.0}) {
    md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", v}}));
  }
  pool.shutdown();

  auto stored = store.get("EXT.SYM", "my_external_rsi");
  ASSERT_TRUE(stored.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*stored), 30.0);
}

TEST(InjectedAtomicTest, MultipleFieldsOnOneTickAllLand) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 71.5},
                                 {"my_external_macd", -0.25},
                                 {"vendor_score", 3.0}}));
  pool.shutdown();

  ASSERT_TRUE(store.get("EXT.SYM", "my_external_rsi").has_value());
  ASSERT_TRUE(store.get("EXT.SYM", "my_external_macd").has_value());
  ASSERT_TRUE(store.get("EXT.SYM", "vendor_score").has_value());
  EXPECT_DOUBLE_EQ(asDouble(*store.get("EXT.SYM", "my_external_macd")), -0.25);
}

// --- Controls: prove real data flow, not an echo. --------------------------

TEST(InjectedAtomicTest, NeverInjectedFieldStaysAbsent) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 71.5}}));
  pool.shutdown();

  EXPECT_FALSE(store.get("EXT.SYM", "never_sent").has_value());
  EXPECT_FALSE(store.get("OTHER.SYM", "my_external_rsi").has_value());

  auto downstream = std::make_shared<Recorder>();
  AtomicAccessor accessor("EXT.SYM", "never_sent", &store, downstream);
  accessor.onValue(StreamValue{"", 0.0});
  EXPECT_EQ(downstream->size(), 0u);
}

TEST(InjectedAtomicTest, NonNumericFieldsAreNotStored) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  auto& alloc = doc->GetAllocator();
  doc->AddMember("venue", rapidjson::Value("NASDAQ", alloc), alloc);
  doc->AddMember("halted", rapidjson::Value(true), alloc);
  doc->AddMember("my_external_rsi", rapidjson::Value(9.5), alloc);
  md.onTick(Event{"EXT.SYM", std::move(doc)});
  pool.shutdown();

  EXPECT_FALSE(store.get("EXT.SYM", "venue").has_value());
  EXPECT_FALSE(store.get("EXT.SYM", "halted").has_value());
  EXPECT_TRUE(store.get("EXT.SYM", "my_external_rsi").has_value());
}

// --- The AC, second half: usable on an interval. ---------------------------

TEST(InjectedAtomicTest, IntervalDrivenAccessorSeesInjectedValue) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 55.0}}));

  auto downstream = std::make_shared<Recorder>();
  auto accessor = std::make_shared<AtomicAccessor>("EXT.SYM", "my_external_rsi",
                                                   &store, downstream);
  auto interval = std::make_shared<Interval>(std::chrono::milliseconds(10),
                                             accessor, nullptr);
  interval->start();

  // Poll rather than sleep-and-assert once, so a slow machine lengthens the
  // test instead of failing it.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (downstream->size() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::size_t afterFirst = downstream->size();

  // A later injection must be visible to the SAME running interval — the value
  // is pulled from the store on every tick, not latched at construction.
  md.onTick(makeTick("EXT.SYM", {{"my_external_rsi", 66.0}}));
  const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool sawUpdate = false;
  while (!sawUpdate && std::chrono::steady_clock::now() < deadline2) {
    for (const auto& sv : downstream->snapshot()) {
      if (std::holds_alternative<double>(sv.value) &&
          std::get<double>(sv.value) == 66.0) {
        sawUpdate = true;
        break;
      }
    }
    if (!sawUpdate) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // shutdown(), not reset(): the timer thread holds a shared_from_this(), so
  // dropping the last external shared_ptr does NOT stop the Interval — it
  // would keep ticking into `accessor` and read `store` after both have been
  // destroyed at end of scope. shutdown() is synchronous and joins the thread.
  interval->shutdown();
  pool.shutdown();

  ASSERT_GT(afterFirst, 0u)
      << "interval fired but the injected value was not readable from the store";
  EXPECT_DOUBLE_EQ(asDouble(downstream->snapshot()[0].value), 55.0);
  EXPECT_TRUE(sawUpdate) << "interval did not observe the updated injected value";
}

// --- Bounds: raw injection is not a second, unbounded way into the store. --
//
// `maxSymbols` is documented as "maximum distinct symbols tracked before
// rejecting new ones". Before ENC-1007 the only path into the AtomicStore ran
// through the Dispatcher's history admission, so that cap held implicitly.
// The raw path has to honour it explicitly — note the store here is
// deliberately left UNCAPPED (main.cpp caps both; an embedder need not), so
// these pin the Dispatcher's own bound rather than AtomicStore::setCaps.

TEST(InjectedAtomicTest, RawInjectionRespectsMaxSymbols) {
  rt::ThreadPool pool(1);
  AtomicStore store;  // intentionally uncapped
  util::Config cfg;
  cfg.maxSymbols = 2;
  Dispatcher md(&pool, &store, cfg);

  md.onTick(makeTick("SYM_A", {{"ext", 1.0}}));
  md.onTick(makeTick("SYM_B", {{"ext", 2.0}}));
  md.onTick(makeTick("SYM_C", {{"ext", 3.0}}));
  pool.shutdown();

  EXPECT_TRUE(store.get("SYM_A", "ext").has_value());
  EXPECT_TRUE(store.get("SYM_B", "ext").has_value());
  EXPECT_FALSE(store.get("SYM_C", "ext").has_value())
      << "raw injection bypassed the maxSymbols admission bound";

  // An already-admitted symbol keeps updating after the cap is reached.
  md.onTick(makeTick("SYM_A", {{"ext", 9.0}}));
  auto a = store.get("SYM_A", "ext");
  ASSERT_TRUE(a.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*a), 9.0);
}

TEST(InjectedAtomicTest, RawInjectionRespectsMaxFieldsPerSymbol) {
  rt::ThreadPool pool(1);
  AtomicStore store;  // intentionally uncapped
  util::Config cfg;
  cfg.maxFieldsPerSymbol = 2;
  Dispatcher md(&pool, &store, cfg);

  md.onTick(makeTick("EXT.SYM", {{"f1", 1.0}}));
  md.onTick(makeTick("EXT.SYM", {{"f2", 2.0}}));
  md.onTick(makeTick("EXT.SYM", {{"f3", 3.0}}));
  pool.shutdown();

  EXPECT_TRUE(store.get("EXT.SYM", "f1").has_value());
  EXPECT_TRUE(store.get("EXT.SYM", "f2").has_value());
  EXPECT_FALSE(store.get("EXT.SYM", "f3").has_value())
      << "raw injection bypassed the maxFieldsPerSymbol admission bound";

  // Admitted fields keep updating once the field cap is reached.
  md.onTick(makeTick("EXT.SYM", {{"f1", 11.0}, {"f3", 33.0}}));
  auto f1 = store.get("EXT.SYM", "f1");
  ASSERT_TRUE(f1.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*f1), 11.0);
  EXPECT_FALSE(store.get("EXT.SYM", "f3").has_value());
}

// --- Guard: the derived-atomic namespace is unchanged by this ticket. ------
//
// Builtin atomics share a FLAT per-symbol namespace keyed by bare function
// name (ENC-792/M9). Writing raw injected fields into the same namespace means
// a field literally named after a builtin would collide. That collision axis
// belongs to ENC-1008; ENC-1007 must not change who wins today, so the derived
// value is pinned as the winner.
TEST(InjectedAtomicTest, DerivedAtomicStillWinsOverASameNamedRawField) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);

  auto listener = std::make_shared<Recorder>();
  md.registerListener("EXT.SYM", "mean", listener);

  // Two ticks, so the derived value and the raw value are DIFFERENT numbers:
  // history [4, 8] -> builtin mean 6.0, while the last raw `mean` field is 8.0.
  // A single tick would make them equal by construction and assert nothing.
  md.onTick(makeTick("EXT.SYM", {{"mean", 4.0}}));
  md.onTick(makeTick("EXT.SYM", {{"mean", 8.0}}));
  pool.shutdown();

  auto stored = store.get("EXT.SYM", "mean");
  ASSERT_TRUE(stored.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*stored), 6.0)
      << "the raw injected field overwrote the derived builtin atomic; "
         "ENC-1007 must leave the derived value as the last writer (ENC-1008 "
         "owns namespacing them apart)";
}
