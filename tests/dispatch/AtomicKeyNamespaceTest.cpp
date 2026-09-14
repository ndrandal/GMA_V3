// ENC-1008 — derived builtin atomics must not clobber each other across fields.
//
// `Dispatcher::computeAndStoreAtomics` historically stored FunctionMap builtins
// under the BARE function name (ENC-792/M9), ignoring the source field. That is
// a single per-symbol slot per builtin, so two fields of one symbol that both
// drive `mean` overwrite each other and one field's value is silently lost.
// ENC-1007 pinned that collision deliberately and handed the fix here
// (tests/dispatch/InjectedAtomicTest.cpp, DerivedAtomicStillWinsOverASameNamedRawField).
//
// The stored key is a CLIENT-VISIBLE string — `field` in a WS subscribe is read
// verbatim by TreeBuilder for both `Listener` (push) and `AtomicAccessor`
// (pull) — and there is no version negotiation on it. So the new `<field>.<fn>`
// shape lives behind `Config::atomicKeyNamespaceByField`, default OFF, and both
// states are pinned here:
//
//   OFF — byte-for-byte the pre-ENC-1008 behaviour, collision included.
//   ON  — each source field gets its own `<field>.<fn>` slot, and Listener push
//         is unchanged so the WS streaming surface does not move.
//
// Field names here are deliberately `alpha`/`beta`, not `price`/`size`: the test
// binary boots a real MarketConnector (tests/test_bootstrap.cpp), and a
// market-shaped field name would additionally be picked up by
// MarketTickComputer and muddy what is being measured.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
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

  // Sorted, because IntegrationTest-style ordering across a thread pool is not
  // guaranteed (ENC-1085) — these arrive via ThreadPool::post.
  std::vector<double> sortedValues() const {
    std::lock_guard<std::mutex> lk(mx_);
    std::vector<double> out;
    for (const auto& sv : received_) {
      if (std::holds_alternative<double>(sv.value)) {
        out.push_back(std::get<double>(sv.value));
      } else if (std::holds_alternative<int>(sv.value)) {
        out.push_back(static_cast<double>(std::get<int>(sv.value)));
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mx_);
    return received_.size();
  }

private:
  mutable std::mutex mx_;
  std::vector<StreamValue> received_;
};

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

util::Config cfgWith(bool namespaced) {
  util::Config c;
  c.atomicKeyNamespaceByField = namespaced;
  return c;
}

// Register a Listener on each raw field. This is what admits the field to the
// per-field history and so drives computeAndStoreAtomics at all.
void registerRawDrivers(Dispatcher& md, const std::string& symbol) {
  md.registerListener(symbol, "alpha", std::make_shared<Recorder>());
  md.registerListener(symbol, "beta", std::make_shared<Recorder>());
}

void tickTwice(Dispatcher& md, const std::string& symbol) {
  md.onTick(makeTick(symbol, {{"alpha", 10.0}, {"beta", 100.0}}));
  md.onTick(makeTick(symbol, {{"alpha", 20.0}, {"beta", 200.0}}));
}

// The shared repro: one symbol, TWO fields, both driving the builtin `mean`.
//
//   alpha history [10, 20]   -> mean  15
//   beta  history [100, 200] -> mean 150
//
// The Listener on the bare `mean` is not decoration: ENC-792/M10 gates the
// compute on something being subscribed, so without it no builtin is ever
// computed or stored and the test would pass vacuously in every state.
void driveTwoFieldRepro(Dispatcher& md,
                        const std::shared_ptr<Recorder>& meanRecorder,
                        const std::string& symbol) {
  registerRawDrivers(md, symbol);
  md.registerListener(symbol, "mean",
                      meanRecorder ? meanRecorder : std::make_shared<Recorder>());
  tickTwice(md, symbol);
}

constexpr double kAlphaMean = 15.0;   // mean of [10, 20]
constexpr double kBetaMean  = 150.0;  // mean of [100, 200]

}  // namespace

// --- The defect, and the fix. ---------------------------------------------

// THE ENC-1008 TEST. Fails on master: neither namespaced key exists, because
// both fields write the single bare `mean` slot and the second one wins.
TEST(AtomicKeyNamespaceTest, TwoFieldsDrivingTheSameBuiltinBothSurvive) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(true));

  driveTwoFieldRepro(md, nullptr, "NS.SYM");
  pool.shutdown();

  auto alpha = store.get("NS.SYM", "alpha.mean");
  auto beta  = store.get("NS.SYM", "beta.mean");

  ASSERT_TRUE(alpha.has_value())
      << "alpha's derived mean was lost — the flat namespace let beta's mean "
         "overwrite it (ENC-1008)";
  ASSERT_TRUE(beta.has_value())
      << "beta's derived mean was lost — the flat namespace let alpha's mean "
         "overwrite it (ENC-1008)";
  EXPECT_DOUBLE_EQ(asDouble(*alpha), kAlphaMean);
  EXPECT_DOUBLE_EQ(asDouble(*beta), kBetaMean);

  // ... and they really are independent slots, not one slot read twice.
  EXPECT_NE(asDouble(*alpha), asDouble(*beta));
}

// The OFF half of the same repro: today's behaviour, collision and all. If this
// ever stops failing to keep both values, the flag's OFF path has drifted.
TEST(AtomicKeyNamespaceTest, FlatNamespaceStillLosesOneFieldsValueWhenOff) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(false));

  driveTwoFieldRepro(md, nullptr, "NS.SYM");
  pool.shutdown();

  // Exactly one slot, and beta is the last writer: onTick fans out in
  // _listeners' std::map order ("alpha" < "beta"), and computeAndStoreAtomics
  // runs inline on this thread, so the write order is deterministic.
  auto flat = store.get("NS.SYM", "mean");
  ASSERT_TRUE(flat.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*flat), kBetaMean)
      << "the flat key must still hold the last field's mean when the flag is "
         "off — OFF is supposed to be the pre-ENC-1008 behaviour unchanged";

  EXPECT_FALSE(store.get("NS.SYM", "alpha.mean").has_value())
      << "namespaced keys must not be written when the flag is off";
  EXPECT_FALSE(store.get("NS.SYM", "beta.mean").has_value())
      << "namespaced keys must not be written when the flag is off";
}

// --- The streaming surface must not move. ---------------------------------

// `field` in a WS subscribe is client-supplied and un-negotiated, so a client
// bound to the bare `mean` has to keep receiving exactly what it receives
// today, in both flag states. Only the AtomicStore key moves.
TEST(AtomicKeyNamespaceTest, BareListenerPushIsIdenticalInBothStates) {
  std::vector<double> off, on;

  for (bool namespaced : {false, true}) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store, cfgWith(namespaced));

    auto rec = std::make_shared<Recorder>();
    driveTwoFieldRepro(md, rec, "NS.SYM");
    pool.shutdown();

    (namespaced ? on : off) = rec->sortedValues();
  }

  // tick 1: alpha [10] -> 10, beta [100] -> 100
  // tick 2: alpha [10,20] -> 15, beta [100,200] -> 150
  const std::vector<double> expected{10.0, 15.0, 100.0, 150.0};
  EXPECT_EQ(off, expected);
  EXPECT_EQ(on, expected)
      << "namespacing the store key changed what a bare-`mean` subscriber "
         "receives; the WS push surface must be identical in both states";
}

// The new key is additionally subscribable, and picks out ONE field.
TEST(AtomicKeyNamespaceTest, NamespacedKeyIsSubscribableAndFieldSpecificWhenOn) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(true));

  auto alphaMean = std::make_shared<Recorder>();
  md.registerListener("NS.SYM", "alpha.mean", alphaMean);
  driveTwoFieldRepro(md, nullptr, "NS.SYM");
  pool.shutdown();

  EXPECT_EQ(alphaMean->sortedValues(), (std::vector<double>{10.0, 15.0}))
      << "a subscriber on `alpha.mean` must see alpha's means only — never "
         "beta's";
}

// ... and does nothing at all when the flag is off, so a client cannot start
// depending on the new shape before the migration happens.
TEST(AtomicKeyNamespaceTest, NamespacedSubscriptionIsInertWhenOff) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(false));

  auto alphaMean = std::make_shared<Recorder>();
  md.registerListener("NS.SYM", "alpha.mean", alphaMean);
  driveTwoFieldRepro(md, nullptr, "NS.SYM");
  pool.shutdown();

  EXPECT_EQ(alphaMean->size(), 0u);
  EXPECT_FALSE(store.get("NS.SYM", "alpha.mean").has_value());
}

// --- What flipping the flag actually breaks, stated as a test. ------------

// The ENC-1007 pin (InjectedAtomicTest.DerivedAtomicStillWinsOverASameNamedRawField)
// asserts the derived builtin is the last writer of the bare key. That is still
// true with the flag off. With the flag on, the derived value vacates the bare
// key entirely and the RAW injected field is what an AtomicAccessor on `mean`
// reads. This is the breaking half of the migration and the reason the default
// is off — so it is asserted rather than left to a comment.
TEST(AtomicKeyNamespaceTest, NamespacingVacatesTheBareKeyForTheRawField) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(true));

  md.registerListener("EXT.SYM", "mean", std::make_shared<Recorder>());
  // history [4, 8] -> builtin mean 6.0, while the last raw `mean` field is 8.0.
  md.onTick(makeTick("EXT.SYM", {{"mean", 4.0}}));
  md.onTick(makeTick("EXT.SYM", {{"mean", 8.0}}));
  pool.shutdown();

  auto bare = store.get("EXT.SYM", "mean");
  ASSERT_TRUE(bare.has_value());
  EXPECT_DOUBLE_EQ(asDouble(*bare), 8.0)
      << "with namespacing on, the derived builtin no longer writes the bare "
         "key, so the raw injected field is the only writer left";

  auto derived = store.get("EXT.SYM", "mean.mean");
  ASSERT_TRUE(derived.has_value())
      << "the derived builtin must still be reachable, under its namespaced key";
  EXPECT_DOUBLE_EQ(asDouble(*derived), 6.0);
}

// The same break, seen through the node a client would actually use.
TEST(AtomicKeyNamespaceTest, AtomicAccessorOnABareBuiltinMovesWhenFlagFlips) {
  for (bool namespaced : {false, true}) {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store, cfgWith(namespaced));

    driveTwoFieldRepro(md, nullptr, "NS.SYM");
    pool.shutdown();

    auto down = std::make_shared<Recorder>();
    AtomicAccessor accessor("NS.SYM", "mean", &store, down);
    accessor.onValue(StreamValue{"", 0.0});

    if (!namespaced) {
      ASSERT_EQ(down->size(), 1u) << "bare `mean` must resolve when the flag is off";
      EXPECT_EQ(down->sortedValues(), (std::vector<double>{kBetaMean}));
    } else {
      EXPECT_EQ(down->size(), 0u)
          << "with the flag on, no builtin writes the bare key and `alpha`/"
             "`beta` are the only raw fields — an AtomicAccessor bound to a "
             "bare builtin name stops resolving. This is the documented "
             "migration break (docs/atomic-keys.md), not an accident.\n"
             "NOTE: `alpha`/`beta` are not market fields, so nothing else "
             "writes the bare key here. On a symbol carrying a price field, "
             "MarketTA also writes bare `mean`/`median`/`spread` and the key "
             "keeps resolving to a DIFFERENT value instead — see "
             "MarketTAKeepsTheBareKeyAliveWithADifferentValue below.";
    }
  }
}

// --- Every builtin moves, not just `mean`. --------------------------------

TEST(AtomicKeyNamespaceTest, NamespacingAppliesToEveryBuiltinNotJustMean) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(true));

  md.registerListener("NS.SYM", "alpha", std::make_shared<Recorder>());
  md.registerListener("NS.SYM", "beta", std::make_shared<Recorder>());
  for (const char* fn : {"sum", "max", "count"}) {
    md.registerListener("NS.SYM", fn, std::make_shared<Recorder>());
  }

  md.onTick(makeTick("NS.SYM", {{"alpha", 10.0}, {"beta", 100.0}}));
  md.onTick(makeTick("NS.SYM", {{"alpha", 20.0}, {"beta", 200.0}}));
  pool.shutdown();

  // ASSERT_TRUE first, deliberately: a regression here would otherwise
  // dereference a disengaged optional and take the whole binary down instead
  // of reporting one red test.
  const std::vector<std::pair<std::string, double>> expected{
      {"alpha.sum", 30.0},  {"beta.sum", 300.0},
      {"alpha.max", 20.0},  {"beta.max", 200.0},
      {"alpha.count", 2.0}, {"beta.count", 2.0},
  };
  for (const auto& [key, want] : expected) {
    auto got = store.get("NS.SYM", key);
    ASSERT_TRUE(got.has_value()) << "missing namespaced key: " << key;
    EXPECT_DOUBLE_EQ(asDouble(*got), want) << "key: " << key;
  }
}

// An unsubscribed builtin is still never computed or stored — the ENC-792/M10
// subscription gate must not have been widened into "compute everything".
TEST(AtomicKeyNamespaceTest, UnsubscribedBuiltinsAreStillNotStoredWhenOn) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store, cfgWith(true));

  registerRawDrivers(md, "NS.SYM");  // note: NO `mean` subscriber
  tickTwice(md, "NS.SYM");
  pool.shutdown();

  EXPECT_FALSE(store.get("NS.SYM", "alpha.stddev").has_value());
  EXPECT_FALSE(store.get("NS.SYM", "stddev").has_value());
  EXPECT_FALSE(store.get("NS.SYM", "alpha.mean").has_value())
      << "nothing subscribed to `mean` or `alpha.mean`, so it must not be "
         "computed at all";
}

// The default really is off — a Dispatcher built with a default-constructed
// Config must behave exactly as it did before this ticket.
TEST(AtomicKeyNamespaceTest, DefaultConfigIsFlatNamespace) {
  EXPECT_FALSE(util::Config{}.atomicKeyNamespaceByField);

  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);  // default Config

  driveTwoFieldRepro(md, nullptr, "NS.SYM");
  pool.shutdown();

  EXPECT_TRUE(store.get("NS.SYM", "mean").has_value());
  EXPECT_FALSE(store.get("NS.SYM", "alpha.mean").has_value());
}

// --- The subtler half of the break: mean / median / spread. ---------------
//
// `mean`, `median` and `spread` are registered FunctionMap builtins AND keys
// that MarketTickComputer writes itself (MarketTA.cpp ~91-92 and ~391), over
// PRICE history rather than over the listened field's history. Today the
// computers run before the listener fan-out in onTick, so the Dispatcher's
// builtin is the last writer and wins the bare key.
//
// With namespacing on the Dispatcher vacates that key, and MarketTA's value —
// a different number, not a missing one — is what an AtomicAccessor on bare
// `mean` reads. So for these three names the migration is a SILENT VALUE
// CHANGE, which is strictly worse to debug than a key that stops resolving,
// and it is the sharpest reason the flag defaults off.
//
// It also splits push from pull: a Listener on bare `mean` still receives the
// Dispatcher's per-field mean while an AtomicAccessor on bare `mean` now reads
// MarketTA's price mean. Under the default they agree.
TEST(AtomicKeyNamespaceTest, MarketTAKeepsTheBareKeyAliveWithADifferentValue) {
  // Listen on `volume`, not on the price field, so the two producers' means
  // are different numbers by construction:
  //   MarketTA   mean over prices  [1, 3, 2]    = 2
  //   Dispatcher mean over volumes [10, 20, 30] = 20
  const auto drive = [](Dispatcher& md) {
    md.registerListener("MKT.SYM", "volume", std::make_shared<Recorder>());
    md.registerListener("MKT.SYM", "mean", std::make_shared<Recorder>());
    const double prices[]  = {1.0, 3.0, 2.0};
    const double volumes[] = {10.0, 20.0, 30.0};
    for (int i = 0; i < 3; ++i) {
      md.onTick(makeTick("MKT.SYM", {{"lastPrice", prices[i]},
                                     {"volume", volumes[i]}}));
    }
  };

  {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store, cfgWith(false));
    drive(md);
    pool.shutdown();

    auto bare = store.get("MKT.SYM", "mean");
    ASSERT_TRUE(bare.has_value());
    EXPECT_DOUBLE_EQ(asDouble(*bare), 20.0)
        << "with the flag off the Dispatcher's builtin must remain the last "
           "writer of the bare key, ahead of MarketTA's price mean";
  }
  {
    rt::ThreadPool pool(1);
    AtomicStore store;
    Dispatcher md(&pool, &store, cfgWith(true));
    drive(md);
    pool.shutdown();

    auto bare = store.get("MKT.SYM", "mean");
    ASSERT_TRUE(bare.has_value())
        << "the bare key does NOT go away here — MarketTA still writes it";
    EXPECT_DOUBLE_EQ(asDouble(*bare), 2.0)
        << "with the flag on, bare `mean` must read MarketTA's price mean; "
           "this is a silent value change for anything bound to it";

    auto ns = store.get("MKT.SYM", "volume.mean");
    ASSERT_TRUE(ns.has_value());
    EXPECT_DOUBLE_EQ(asDouble(*ns), 20.0)
        << "the Dispatcher's per-field mean must be reachable under its "
           "namespaced key";
  }
}
