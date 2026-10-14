// ENC-1486 — Dispatcher::onRecord reaches the same place onTick does.
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D3, D9;
//         docs/data-layer.md §C2, §C6, §C7, §6.
//
// WHAT "IDENTICAL" HAS TO MEAN HERE
// ---------------------------------
// SPEC D1 is "one implementation of every operator". The record path exists to
// remove a rapidjson DOM from ingress, and the whole value of that is that it
// removes ONLY the DOM: a graph must not be able to tell which entry point fed
// it. "Equivalent" is not enough — two implementations of the Dispatcher's
// routing would agree on the easy cases and diverge on the ones that matter
// (field order, admission caps, which builtins fire, whether a derived atomic
// overwrites a raw one). So this file asserts IDENTICAL, in three senses:
//
//   1. the same listener deliveries, IN THE SAME ORDER, with bit-equal values;
//   2. the same AtomicStore contents, over a key set DERIVED from the engine
//      rather than hardcoded here;
//   3. the same IEventComputer invocations, with the same fields visible.
//
// Ordering is load-bearing and is a documented contract, not an accident:
// Dispatcher.hpp records single-threaded ingress as a PRECONDITION of
// ENC-1005's ordering, `deliver()` preserves order by calling strand-bearing
// listeners inline, and `_listeners`' inner container is a `std::map<field,…>`
// so `ask` precedes `bid`. Both rigs here run a ONE-THREAD pool (so posts
// execute in post order, docs/data-layer.md §C7) and end on `drain()`, never a
// sleep — a timer node posts rather than calls, and a longer sleep is a wider
// window, not a fix (ENC-1340).
//
// HOW THIS TEST AVOIDS BEING A GATE THAT CANNOT SEE
// ------------------------------------------------
// A parity test that has never been observed failing has not been shown to
// test parity. Two independent defences, both permanent:
//
//   * NEGATIVE CONTROLS. `ComparatorSeesA*Divergence` drive the two paths with
//     deliberately DIFFERENT input and assert the comparison reports it — one
//     for a changed value, one for an extra field, one for a changed
//     field-emission order. If the comparator ever goes blind, those three go
//     red, every run, without anyone remembering to re-check.
//   * The comparison is over a key set derived from `FunctionMap`,
//     `market::computeAllAtomicValues` and `RecordFieldSource` itself, so a
//     new field or a new builtin enters the comparison automatically instead
//     of quietly falling outside it.
//
// The one-off proof that the PRODUCTION path is what is being compared — a
// perturbation of `Dispatcher::ingest` reddening `DeliveriesAreIdenticalInOrder`
// and `AtomicStoreContentsAreIdentical` — is recorded on ENC-1486.
//
// NO PROCESS-GLOBAL REGISTRY IS WRITTEN BY THIS FILE (ENC-1102). The computers
// here go in through `Dispatcher::addComputer`, which is per-dispatcher state,
// and the listeners are raw `INode`s registered directly — no `NodeTypeRegistry`,
// no `EventComputerRegistry`, no `FunctionMap` writes. So `--gtest_repeat`
// against the full binary is unaffected by it.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/MarketTA.hpp"
#include "gma/StreamValue.hpp"
#include "gma/SymbolHistory.hpp"
#include "gma/data/Record.hpp"
#include "gma/data/SymbolTable.hpp"
#include "gma/engine/FieldSource.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/engine/JsonFieldSource.hpp"
#include "gma/engine/RecordFieldSource.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gma;

namespace {

// ---------------------------------------------------------------------------
// Observation
// ---------------------------------------------------------------------------

// One delivery, rendered so a mismatch prints something a human can read.
// The value goes through "%.17g": that round-trips an IEEE double exactly, so
// string equality here IS bit equality, without the test having to care which
// ArgType alternative a node happened to produce.
std::string describe(const ArgType& v);

std::string describeDouble(double d) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", d);
  return buf;
}

std::string describe(const ArgType& v) {
  return std::visit([](auto&& x) -> std::string {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, double>) return "d:" + describeDouble(x);
    else if constexpr (std::is_same_v<T, int>) return "i:" + std::to_string(x);
    else if constexpr (std::is_same_v<T, bool>) return x ? "b:1" : "b:0";
    else if constexpr (std::is_same_v<T, std::string>) return "s:" + x;
    else if constexpr (std::is_same_v<T, std::vector<int>>) {
      std::string s = "vi:";
      for (int e : x) s += std::to_string(e) + ",";
      return s;
    } else if constexpr (std::is_same_v<T, std::vector<double>>) {
      std::string s = "vd:";
      for (double e : x) s += describeDouble(e) + ",";
      return s;
    } else if constexpr (std::is_same_v<T, Record>) {
      std::string s = "rec:{";
      for (const auto& f : x.fields) s += f.name + "=" + describe(f.value.value) + ",";
      return s + "}";
    } else {
      std::string s = "arr:[";
      for (const auto& e : x) s += describe(e.value) + ",";
      return s + "]";
    }
  }, v);
}

// The ordered delivery log of one rig. Every Recorder writes into the same log,
// so the log captures CROSS-FIELD order (`ask` before `bid`) and not just
// per-field order.
class DeliveryLog {
public:
  void append(const std::string& label, const StreamValue& sv) {
    std::lock_guard<std::mutex> lk(mx_);
    rows_.push_back(label + " | " + sv.symbol + " | " + describe(sv.value) +
                    " | bucket=" + std::to_string(sv.bucketStartMs));
  }
  std::vector<std::string> rows() const {
    std::lock_guard<std::mutex> lk(mx_);
    return rows_;
  }
private:
  mutable std::mutex       mx_;
  std::vector<std::string> rows_;
};

// A plain INode. `deliversOnOwnExecutor()` stays false, so every delivery goes
// through `deliver()`'s ThreadPool::post branch — the production routing for a
// node without a strand — rather than an inline call that would hide a
// scheduling difference between the two paths.
class Recorder final : public INode {
public:
  Recorder(std::string label, DeliveryLog* log)
    : label_(std::move(label)), log_(log) {}
  void onValue(const StreamValue& sv) override { log_->append(label_, sv); }
  void shutdown() noexcept override {}
private:
  std::string  label_;
  DeliveryLog* log_;
};

// Records what the computers saw. Proves "the same IEventComputers" directly
// instead of inferring it from the atomics they happen to write.
class ComputerProbe final : public engine::IEventComputer {
public:
  ComputerProbe(std::string type, std::vector<std::string>* sink)
    : type_(std::move(type)), sink_(sink) {}

  std::string_view eventType() const override { return type_; }

  void compute(const std::string& symbol,
               const engine::FieldSource& fields,
               engine::ComputeContext& ctx) override {
    // Everything a computer can observe about its input, flattened.
    struct Dump final : engine::NumericFieldSink {
      std::string out;
      void field(const std::string& name, double value) override {
        out += name + "=" + describeDouble(value) + ";";
      }
    } dump;
    fields.forEachNumber(dump);
    sink_->push_back("compute " + symbol +
                     " isObject=" + (fields.isObject() ? "1" : "0") +
                     " ctx=" + (ctx.store ? "s" : "-") +
                     (ctx.dispatcher ? "d" : "-") + (ctx.pool ? "p" : "-") +
                     " fields={" + dump.out + "}");
  }

private:
  std::string               type_;
  std::vector<std::string>* sink_;
};

// ---------------------------------------------------------------------------
// The rig — one Dispatcher, one store, one single-threaded pool
// ---------------------------------------------------------------------------

constexpr char kTicker[] = "PARITY";

// The fields a listener is registered on. Deliberately a mix:
//   * raw record fields (`price`, `size`, `bid`, `ask`) — the direct-field
//     fan-out, where the `std::map` ordering contract lives;
//   * FunctionMap builtins (`mean`, `sum`, `stddev`) — `computeAndStoreAtomics`
//     only computes a builtin that has a subscriber (ENC-792/M10), so without
//     these the derived half of the path is never exercised;
//   * MarketTA pushes (`lastPrice`, `sma_5`) — `notifyListeners` from inside a
//     connector computer, which is the third of the three delivery routes.
const std::vector<std::string>& listenedFields() {
  static const std::vector<std::string> f = {
      "ask", "bid", "price", "size", "mean", "sum", "stddev", "lastPrice", "sma_5"};
  return f;
}

struct Rig {
  explicit Rig(const util::Config& cfg)
    : pool(1), disp(&pool, &store, cfg) {
    for (const auto& field : listenedFields()) {
      auto node = std::make_shared<Recorder>(field, &log);
      nodes.push_back(node);
      disp.registerListener(kTicker, field, node);
    }
    disp.addComputer(std::make_unique<ComputerProbe>("tick", &computerLog));
    symbols.intern("ZZ-PAD");      // burn id 1 so kTicker is not id 1 by luck
    symbolId = symbols.intern(kTicker);
    disp.setSymbolTable(&symbols);
  }

  // The deterministic barrier. `drain()` waits for the queue to empty AND for
  // in-flight tasks to finish; a sleep would be a window (ENC-1340).
  void quiesce() { pool.drain(); }

  rt::ThreadPool                      pool;
  AtomicStore                         store;
  data::SymbolTable                   symbols;
  data::SymbolId                      symbolId = 0;
  Dispatcher                          disp;
  DeliveryLog                         log;
  std::vector<std::string>            computerLog;
  std::vector<std::shared_ptr<INode>> nodes;
};

// ---------------------------------------------------------------------------
// The tick corpus, and the JSON tick DERIVED from it
// ---------------------------------------------------------------------------

data::TickRecord makeRecord(data::SymbolId sym, double price, std::uint64_t size,
                            double bid, double ask) {
  data::TickRecord r;
  r.header.symbolId   = sym;
  r.header.venueId    = 7;
  r.header.tsEventNs  = 1'700'000'000'000'000'000LL;
  r.header.tsRecvNs   = 1'700'000'000'000'000'123LL;
  r.header.providerSeq = 42;
  r.header.sessionId  = 3;
  r.kind      = data::RecordKind::Trade;
  r.price     = data::toFixed(price);
  r.size      = size;
  r.aggressor = data::Aggressor::Buy;
  r.tradeId   = 99;
  r.bidPx     = data::toFixed(bid);
  r.askPx     = data::toFixed(ask);
  r.bidSz     = 11;
  r.askSz     = 13;
  return r;
}

// THE EQUIVALENT TICK, derived from the record through the SAME mapping the
// record path uses — `RecordFieldSource` itself — and never restated here.
//
// That is the point. If the mapping in
// include/gma/engine/RecordFieldSource.hpp gains a field, both sides of this
// test gain it together, so this test keeps measuring what it claims to
// measure (do the two ENTRY POINTS agree) instead of silently becoming a test
// of a hardcoded field list that has drifted. What the mapping SHOULD be is a
// decision, documented where it is made; it is not what this file pins.
Event equivalentEvent(const std::string& symbol, const data::TickRecord& rec) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  struct AddMembers final : engine::NumericFieldSink {
    rapidjson::Document* doc;
    void field(const std::string& name, double value) override {
      auto& alloc = doc->GetAllocator();
      doc->AddMember(rapidjson::Value(name.c_str(),
                                      static_cast<rapidjson::SizeType>(name.size()),
                                      alloc),
                     rapidjson::Value(value), alloc);
    }
  } add;
  add.doc = doc.get();
  const engine::RecordFieldSource fields(rec);
  fields.forEachNumber(add);
  return Event{symbol, std::move(doc)};
}

// Four ticks, not one. A single tick cannot see a history buffer, a rolling
// builtin or the cap bookkeeping; `sma_5` needs depth and `stddev` of one
// sample is not a test of anything.
std::vector<data::TickRecord> corpus(data::SymbolId sym) {
  return {
      makeRecord(sym, 100.25,  10, 100.20, 100.30),
      makeRecord(sym, 100.50,  25, 100.45, 100.55),
      makeRecord(sym,  99.75, 300,  99.70,  99.80),
      makeRecord(sym, 101.00,   1, 100.95, 101.05),
  };
}

// ---------------------------------------------------------------------------
// The AtomicStore comparison, over a DERIVED key set
// ---------------------------------------------------------------------------

// `AtomicStore` has no enumeration accessor, so the comparison needs a
// candidate key list. Hardcoding one is how a store comparison silently stops
// covering a key, so every key here is derived from the engine:
//
//   * every field `RecordFieldSource` can emit, from the source itself;
//   * every `FunctionMap` builtin, bare AND `<field>.<fn>` (the ENC-1008 key
//     shape), for every raw field;
//   * every key `market::computeAllAtomicValues` writes, by running it on a
//     scratch store — which is the whole TA suite under the active Config,
//     without this file naming one indicator;
//   * MarketTickComputer's own base atomics.
std::set<std::string> candidateKeys(const util::Config& cfg,
                                    const data::TickRecord& sample) {
  std::set<std::string> keys;

  std::vector<std::string> rawFields;
  {
    struct Collect final : engine::NumericFieldSink {
      std::vector<std::string>* out;
      void field(const std::string& name, double) override { out->push_back(name); }
    } collect;
    collect.out = &rawFields;
    const engine::RecordFieldSource fields(sample);
    fields.forEachNumber(collect);
  }
  for (const auto& f : rawFields) keys.insert(f);
  for (const auto& f : listenedFields()) keys.insert(f);

  FunctionMap::instance().forEach([&](const std::string& fn, const Func&) {
    keys.insert(fn);
    for (const auto& f : rawFields) keys.insert(f + "." + fn);
  });

  // MarketTickComputer's base atomics (MarketTA.cpp, "Symbol admitted" block).
  keys.insert("bid");
  keys.insert("ask");
  keys.insert("spread");
  keys.insert("timestamp");

  // The TA suite, enumerated by running it rather than by listing it.
  {
    AtomicStore scratch;
    std::vector<TickEntry> hist;
    for (int i = 0; i < 8; ++i) {
      hist.push_back(TickEntry{100.0 + i, 10.0 + i, 99.0 + i, 101.0 + i, 0});
    }
    for (const auto& [k, v] : computeAllAtomicValues("SCRATCH", hist,
                                                             scratch, cfg)) {
      (void)v;
      keys.insert(k);
    }
  }
  return keys;
}

// Renders a store as one line per present key, for the candidate set.
std::vector<std::string> dumpStore(const AtomicStore& store,
                                   const std::set<std::string>& keys) {
  std::vector<std::string> out;
  for (const auto& k : keys) {
    auto v = store.get(kTicker, k);
    if (!v) continue;
    out.push_back(k + " = " + describe(*v));
  }
  return out;
}

// ---------------------------------------------------------------------------
// Drive
// ---------------------------------------------------------------------------

// Everything the two paths can be compared on, from one rig.
struct Observed {
  std::vector<std::string> deliveries;
  std::vector<std::string> store;
  std::vector<std::string> computers;
};

enum class Path { Json, Record };

// `mutate` runs on each record before it is driven, so a negative control can
// perturb ONE path's input while leaving the drive identical.
Observed drive(Path path, const util::Config& cfg,
               void (*mutate)(data::TickRecord&) = nullptr) {
  Rig rig(cfg);
  auto recs = corpus(rig.symbolId);
  for (auto& rec : recs) {
    if (mutate) mutate(rec);
    if (path == Path::Record) {
      rig.disp.onRecord(rec);
    } else {
      rig.disp.onTick(equivalentEvent(kTicker, rec));
    }
  }
  rig.quiesce();

  Observed o;
  o.deliveries = rig.log.rows();
  o.store      = dumpStore(rig.store, candidateKeys(cfg, recs.front()));
  o.computers  = rig.computerLog;
  return o;
}

util::Config defaultCfg() {
  util::Config cfg;
  cfg.threadPoolSize = 1;   // docs/data-layer.md §C7
  return cfg;
}

} // namespace

// ===========================================================================
// 1. The parity assertions
// ===========================================================================

TEST(RecordIngressParityTest, DeliveriesAreIdenticalInOrder) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg);

  // ANTI-VACUITY. A comparison of two empty logs passes every predicate below
  // and proves nothing, which is exactly what a parity gate looks like when it
  // has stopped working.
  ASSERT_FALSE(json.deliveries.empty())
      << "the JSON path delivered nothing — the comparison below is vacuous";

  // Order, not membership: element-wise, same length, same sequence.
  EXPECT_EQ(json.deliveries, rec.deliveries);

  // The `std::map<field,…>` ordering contract on Dispatcher.hpp, asserted
  // rather than assumed: within one tick, `ask` precedes `bid`.
  const auto firstOf = [](const std::vector<std::string>& rows,
                          const std::string& label) -> int {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].rfind(label + " |", 0) == 0) return static_cast<int>(i);
    }
    return -1;
  };
  ASSERT_GE(firstOf(rec.deliveries, "ask"), 0);
  ASSERT_GE(firstOf(rec.deliveries, "bid"), 0);
  EXPECT_LT(firstOf(rec.deliveries, "ask"), firstOf(rec.deliveries, "bid"));
}

TEST(RecordIngressParityTest, AtomicStoreContentsAreIdentical) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg);

  ASSERT_FALSE(json.store.empty())
      << "the JSON path wrote no atomic — the comparison below is vacuous";
  // Enough keys to be a real comparison: the raw fields, the subscribed
  // builtins, and the TA suite MarketTickComputer writes.
  ASSERT_GT(json.store.size(), 10u);
  EXPECT_EQ(json.store, rec.store);
}

TEST(RecordIngressParityTest, ComputerInvocationsAreIdentical) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg);

  ASSERT_EQ(json.computers.size(), corpus(1).size())
      << "the probe computer was not invoked once per tick on the JSON path";
  EXPECT_EQ(json.computers, rec.computers);
}

TEST(RecordIngressParityTest, RecordsRouteToTheTickComputerSet) {
  // A record must reach the computers an `Event{sym, payload}` reaches —
  // `Event::type` defaults to "tick" and MarketConnector registers there.
  EXPECT_EQ(Dispatcher::recordEventType(), Event{}.type);
  EXPECT_EQ(Dispatcher::recordEventType(), "tick");
}

TEST(RecordIngressParityTest, NoFieldAnswersTryGetUint64OnEitherPath) {
  // The ONE place the two FieldSources could legitimately have disagreed, and
  // it is reachable from a config file rather than from code.
  //
  // `MarketTickComputer` reads `MarketFieldMap::timestampField` as an exact
  // `u64`, because a nanosecond epoch does not survive a `double`. rapidjson
  // answers `IsUint64()` only for a value STORED as an integer, so a field map
  // naming a size column (`market.source.timestampField = size`) would read a
  // timestamp on whichever path happened to carry exact integers and not on
  // the other. `RecordFieldSource::tryGetUint64` therefore refuses every
  // field, which makes the two paths identical for EVERY field map instead of
  // only the default one.
  //
  // Asserted at the FieldSource level rather than by driving a Dispatcher,
  // because the field map belongs to the connector's registry factory and
  // rewriting it would mean writing a process-global registry (ENC-1102).
  const data::TickRecord rec = makeRecord(1, 100.25, 10, 100.20, 100.30);
  const engine::RecordFieldSource recFields(rec);
  const Event ev = equivalentEvent(kTicker, rec);
  const engine::JsonFieldSource jsonFields(ev.payload.get());

  std::vector<std::string> names;
  {
    struct Collect final : engine::NumericFieldSink {
      std::vector<std::string>* out;
      void field(const std::string& name, double) override { out->push_back(name); }
    } collect;
    collect.out = &names;
    recFields.forEachNumber(collect);
  }
  ASSERT_FALSE(names.empty()) << "the record emitted no field — vacuous";

  for (const auto& n : names) {
    std::uint64_t a = 1, b = 1;
    EXPECT_FALSE(recFields.tryGetUint64(n, a)) << n;
    EXPECT_FALSE(jsonFields.tryGetUint64(n, b)) << n;
    // And both still read as numbers, so nothing is simply missing.
    double da = 0.0, db = 0.0;
    EXPECT_TRUE(recFields.tryGetNumber(n, da)) << n;
    EXPECT_TRUE(jsonFields.tryGetNumber(n, db)) << n;
    EXPECT_EQ(describeDouble(da), describeDouble(db)) << n;
  }
}

// ===========================================================================
// 2. NEGATIVE CONTROLS — the comparator is shown to fail on a divergence
// ===========================================================================
//
// Without these, every assertion above is consistent with a comparator that
// cannot see anything. Each test below perturbs ONE path's input and requires
// the comparison to notice.

TEST(RecordIngressParityTest, ComparatorSeesAValueDivergence) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg, [](data::TickRecord& r) {
    r.price += 1;   // one unit at 1e-9 — the smallest change the schema can express
  });
  EXPECT_NE(json.deliveries, rec.deliveries);
  EXPECT_NE(json.store, rec.store);
  EXPECT_NE(json.computers, rec.computers);
}

TEST(RecordIngressParityTest, ComparatorSeesAMissingField) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg, [](data::TickRecord& r) {
    r.size = 0;     // suppresses the "size" field entirely (see the mapping)
  });
  EXPECT_NE(json.deliveries, rec.deliveries);
  EXPECT_NE(json.computers, rec.computers);
}

TEST(RecordIngressParityTest, ComparatorSeesAQuoteSideDisappearing) {
  const auto cfg = defaultCfg();
  const Observed json = drive(Path::Json, cfg);
  const Observed rec  = drive(Path::Record, cfg, [](data::TickRecord& r) {
    r.bidPx = data::kNoPrice;   // drops "bid" and "bidSize"
  });
  EXPECT_NE(json.deliveries, rec.deliveries);
  EXPECT_NE(json.store, rec.store);
}

// ===========================================================================
// 3. STRUCTURAL — the record path names no JSON type
// ===========================================================================
//
// "No rapidjson::Document is constructed on this path" cannot be asserted by
// running the code: a DOM built and discarded leaves no observable trace in a
// delivery log. So it is asserted over the SOURCE of the record path, with a
// positive control in the same file, read by the same reader, against the same
// token list — because a negative from a search that has not been seen to
// return a true positive is not a result (the workspace-root grep that reaches
// none of the seven repos is the same bug).
//
// Comments and string literals are stripped before the search. Prose about
// JSON is not a JSON dependency, and a token-counting gate that cannot tell
// the difference is one this workspace has already been bitten by.

namespace {

std::string readSource(const std::string& relative) {
  const std::string path = std::string(GMA_TEST_SOURCE_DIR) + "/" + relative;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  // ENC-1233 §2 D10: a missing fixture is a failure, never a skip.
  if (!f) return {};
  std::string out;
  char buf[8192];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

// Replaces every comment and string/char literal with spaces, so what is left
// is only what the translation unit NAMES.
std::string scrubComments(const std::string& src) {
  std::string out(src.size(), ' ');
  enum class S { Code, Line, Block, Str, Chr } st = S::Code;
  for (std::size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
    switch (st) {
      case S::Code:
        if (c == '/' && n == '/') { st = S::Line; }
        else if (c == '/' && n == '*') { st = S::Block; }
        else if (c == '"') { st = S::Str; }
        else if (c == '\'') { st = S::Chr; }
        else out[i] = c;
        break;
      case S::Line:
        if (c == '\n') { st = S::Code; out[i] = c; }
        break;
      case S::Block:
        if (c == '*' && n == '/') { st = S::Code; ++i; }
        else if (c == '\n') out[i] = c;
        break;
      case S::Str:
        if (c == '\\') ++i;
        else if (c == '"') st = S::Code;
        break;
      case S::Chr:
        if (c == '\\') ++i;
        else if (c == '\'') st = S::Code;
        break;
    }
  }
  return out;
}

// The body of `Dispatcher::<name>` in already-scrubbed source, brace-matched.
// Empty when the function is not found, which every caller treats as a failure
// rather than as "nothing to check".
std::string functionBody(const std::string& scrubbed, const std::string& name) {
  const std::string sig = "Dispatcher::" + name + "(";
  const std::size_t at = scrubbed.find(sig);
  if (at == std::string::npos) return {};
  const std::size_t open = scrubbed.find('{', at);
  if (open == std::string::npos) return {};
  int depth = 0;
  for (std::size_t i = open; i < scrubbed.size(); ++i) {
    if (scrubbed[i] == '{') ++depth;
    else if (scrubbed[i] == '}') {
      if (--depth == 0) return scrubbed.substr(open, i - open + 1);
    }
  }
  return {};
}

const std::vector<std::string>& jsonTokens() {
  static const std::vector<std::string> t = {
      "rapidjson", "Document", "payload", "HasMember", "MemberBegin",
      "MemberEnd", "GetDouble", "GetUint64", "JsonFieldSource"};
  return t;
}

// Matches a token only where it STARTS an identifier. Plain substring search
// was wrong in a way worth recording, because it is the shape of a false
// positive this gate would otherwise have shipped: `GetUint64` is a substring
// of `tryGetUint64`, the FieldSource accessor, so RecordFieldSource.hpp
// "named rapidjson" on its first run. Requiring the preceding character to be
// a non-identifier one keeps `value.GetUint64()` (preceded by `.`) a hit and
// `tryGetUint64` (preceded by `y`) a miss.
bool identStart(const std::string& hay, std::size_t at) {
  if (at == 0) return true;
  const char p = hay[at - 1];
  return !(std::isalnum(static_cast<unsigned char>(p)) || p == '_');
}

std::vector<std::string> jsonTokensIn(const std::string& scrubbed) {
  std::vector<std::string> found;
  for (const auto& tok : jsonTokens()) {
    for (std::size_t at = scrubbed.find(tok); at != std::string::npos;
         at = scrubbed.find(tok, at + 1)) {
      if (identStart(scrubbed, at)) { found.push_back(tok); break; }
    }
  }
  return found;
}

}  // namespace

TEST(RecordPathIsJsonFreeTest, PositiveControlTheJsonPathDoesNameJson) {
  const std::string src = readSource("src/core/Dispatcher.cpp");
  ASSERT_FALSE(src.empty()) << "src/core/Dispatcher.cpp not readable from "
                            << GMA_TEST_SOURCE_DIR;
  const std::string body = functionBody(scrubComments(src), "onTick");
  ASSERT_FALSE(body.empty()) << "could not locate Dispatcher::onTick — the "
                                "negative verdicts below would be vacuous";
  // If this ever goes green-by-emptiness, every other test in this suite is
  // measuring nothing.
  EXPECT_FALSE(jsonTokensIn(body).empty())
      << "the JSON ingress path names no JSON token — the scrubber or the "
         "token list is broken, not the code";
}

TEST(RecordPathIsJsonFreeTest, SharedIngestBodyNamesNoJson) {
  const std::string src = readSource("src/core/Dispatcher.cpp");
  ASSERT_FALSE(src.empty());
  const std::string body = functionBody(scrubComments(src), "ingest");
  ASSERT_FALSE(body.empty()) << "could not locate Dispatcher::ingest";
  ASSERT_GT(body.size(), 500u) << "Dispatcher::ingest body implausibly short — "
                                  "brace matching probably failed";
  const auto found = jsonTokensIn(body);
  EXPECT_TRUE(found.empty())
      << "Dispatcher::ingest is the body BOTH entry points share; it must be "
         "JSON-free. Found: " << (found.empty() ? std::string{} : found[0]);
}

TEST(RecordPathIsJsonFreeTest, RecordPathTranslationUnitNamesNoJson) {
  const std::string src = readSource("src/core/DispatcherRecord.cpp");
  ASSERT_FALSE(src.empty()) << "src/core/DispatcherRecord.cpp not readable";
  const std::string scrubbed = scrubComments(src);
  const std::string body = functionBody(scrubbed, "onRecord");
  ASSERT_FALSE(body.empty()) << "could not locate Dispatcher::onRecord";

  auto found = jsonTokensIn(body);
  EXPECT_TRUE(found.empty())
      << "Dispatcher::onRecord names a JSON type: "
      << (found.empty() ? std::string{} : found[0]);

  // The whole TU, not just the one function: a helper beside it would be on
  // the same path.
  found = jsonTokensIn(scrubbed);
  EXPECT_TRUE(found.empty())
      << "src/core/DispatcherRecord.cpp names a JSON type: "
      << (found.empty() ? std::string{} : found[0]);
}

TEST(RecordPathIsJsonFreeTest, RecordFieldSourceHeaderNamesNoJsonButJsonOneDoes) {
  const std::string rec = readSource("include/gma/engine/RecordFieldSource.hpp");
  const std::string jsn = readSource("include/gma/engine/JsonFieldSource.hpp");
  ASSERT_FALSE(rec.empty());
  ASSERT_FALSE(jsn.empty());

  // Positive control, same reader and same token list.
  EXPECT_FALSE(jsonTokensIn(scrubComments(jsn)).empty())
      << "JsonFieldSource.hpp names no JSON token — the check is broken";

  const auto found = jsonTokensIn(scrubComments(rec));
  EXPECT_TRUE(found.empty())
      << "RecordFieldSource.hpp names a JSON type: "
      << (found.empty() ? std::string{} : found[0]);
}

// ===========================================================================
// 4. THE MAPPING, PINNED ABSOLUTELY
// ===========================================================================
//
// WHY THE PARITY TESTS ABOVE ARE NOT ENOUGH, measured rather than assumed.
//
// `equivalentEvent` derives the JSON tick from the record through
// `RecordFieldSource` — deliberately, so a new field enters both sides of the
// comparison together and the parity test keeps testing the two ENTRY POINTS
// instead of slowly becoming a test of a stale field list. The cost of that
// choice is exact and was measured by perturbing the shipped code:
//
//   perturbation of RecordFieldSource          parity tests
//   -----------------------------------------  --------------------------
//   stop emitting `size`                       GREEN (control caught it)
//   `toDouble(fixed + 1)` on every price       GREEN
//   swap the emission order of `bid`/`ask`     GREEN
//
// A mapping change lands on BOTH sides, so a relative comparison cannot see
// it. (The first row did redden `ComparatorSeesAMissingField`, because that
// negative control's own perturbation stopped being a divergence — the suite
// noticed, which is the point of having it, but it named the wrong thing.)
//
// So the mapping is pinned here ABSOLUTELY: field names, values and emission
// ORDER, against literals. Parity keeps the two paths honest about each other;
// this keeps the mapping honest about the record. All three perturbations
// above redden this suite.

namespace {

// `name=value;` in emission order — the full observable content of a source.
std::string flatten(const engine::FieldSource& fields) {
  struct Dump final : engine::NumericFieldSink {
    std::string out;
    void field(const std::string& name, double value) override {
      out += name + "=" + describeDouble(value) + ";";
    }
  } dump;
  fields.forEachNumber(dump);
  return dump.out;
}

data::TickRecord bare(data::RecordKind kind) {
  data::TickRecord r;
  r.header.symbolId = 1;
  r.kind = kind;
  return r;
}

}  // namespace

TEST(RecordFieldSourceMappingTest, TradeWithBothQuoteSides) {
  data::TickRecord r = bare(data::RecordKind::Trade);
  r.price = data::toFixed(100.25);
  r.size  = 10;
  r.bidPx = data::toFixed(100.20);
  r.askPx = data::toFixed(100.30);
  r.bidSz = 11;
  r.askSz = 13;

  // Order is part of the contract: price, size, bid, ask, bidSize, askSize.
  EXPECT_EQ(flatten(engine::RecordFieldSource(r)),
            "price=100.25;size=10;bid=100.2;ask=100.3;bidSize=11;askSize=13;");
}

TEST(RecordFieldSourceMappingTest, TradeOnly) {
  data::TickRecord r = bare(data::RecordKind::Trade);
  r.price = data::toFixed(42.5);
  r.size  = 7;
  EXPECT_EQ(flatten(engine::RecordFieldSource(r)), "price=42.5;size=7;");
}

TEST(RecordFieldSourceMappingTest, QuoteKindSuppressesPriceAndSize) {
  // One struct serves a trade and an L1 quote; `kind` says which half is
  // meaningful (include/gma/data/Record.hpp).
  data::TickRecord r = bare(data::RecordKind::Quote);
  r.price = data::toFixed(42.5);   // set, and must NOT be emitted
  r.size  = 7;                     // ditto
  r.bidPx = data::toFixed(42.0);
  r.askPx = data::toFixed(43.0);
  r.bidSz = 100;
  r.askSz = 200;
  EXPECT_EQ(flatten(engine::RecordFieldSource(r)),
            "bid=42;ask=43;bidSize=100;askSize=200;");
}

TEST(RecordFieldSourceMappingTest, AbsentAndZeroFieldsAreOmittedNotZeroed) {
  // A default record names no symbol and carries kNoPrice everywhere, so it
  // must emit NOTHING — not a row of zeros, which would clock every listener
  // and push a 0 into every history buffer.
  EXPECT_EQ(flatten(engine::RecordFieldSource(bare(data::RecordKind::Trade))), "");

  // A trade with a price but no size emits one field.
  data::TickRecord r = bare(data::RecordKind::Trade);
  r.price = data::toFixed(1.5);
  EXPECT_EQ(flatten(engine::RecordFieldSource(r)), "price=1.5;");

  // A quote with one side only. bidSize is gated on bidPx being present, so a
  // size with no price beside it is not emitted on its own.
  data::TickRecord q = bare(data::RecordKind::Quote);
  q.askPx = data::toFixed(9.0);
  q.askSz = 5;
  q.bidSz = 77;            // bidPx is kNoPrice -> bidSize is suppressed
  EXPECT_EQ(flatten(engine::RecordFieldSource(q)), "ask=9;askSize=5;");
}

TEST(RecordFieldSourceMappingTest, PriceDecodesExactlyAtTheGlobalScale) {
  // SPEC D8: Int64 at a fixed global 1e-9. A price read must be the record's
  // integer divided by exactly that, with no per-symbol tick size involved.
  data::TickRecord r = bare(data::RecordKind::Trade);
  r.price = 1'234'567'890LL;             // 1.23456789 at 1e-9
  const engine::RecordFieldSource src(r);
  double px = 0.0;
  ASSERT_TRUE(src.tryGetNumber(engine::recordfields::kPrice, px));
  EXPECT_EQ(describeDouble(px), describeDouble(1.23456789));

  // Negative prices are legal (OrderBookManager::setAllowNegativePrices) and
  // kNoPrice is INT64_MIN rather than 0 for that reason.
  r.price = -1'500'000'000LL;
  double neg = 0.0;
  ASSERT_TRUE(engine::RecordFieldSource(r).tryGetNumber(
      engine::recordfields::kPrice, neg));
  EXPECT_EQ(describeDouble(neg), describeDouble(-1.5));
}

TEST(RecordFieldSourceMappingTest, LookupAgreesWithEmission) {
  // `has`/`tryGetNumber` and `forEachNumber` must describe the same field set.
  // They are separate code paths over one array, and the Dispatcher uses BOTH
  // (admission enumerates, listener fan-out looks up), so a disagreement would
  // admit a field that no listener can read or the reverse.
  data::TickRecord r = bare(data::RecordKind::Trade);
  r.price = data::toFixed(5.0);
  r.size  = 2;
  r.askPx = data::toFixed(5.5);
  const engine::RecordFieldSource src(r);

  std::vector<std::string> emitted;
  {
    struct Collect final : engine::NumericFieldSink {
      std::vector<std::string>* out;
      void field(const std::string& name, double) override { out->push_back(name); }
    } c;
    c.out = &emitted;
    src.forEachNumber(c);
  }
  ASSERT_EQ(emitted.size(), 3u);
  for (const auto& n : emitted) EXPECT_TRUE(src.has(n)) << n;

  for (const auto& n : {std::string("bid"), std::string("bidSize"),
                        std::string("nonsense")}) {
    EXPECT_FALSE(src.has(n)) << n;
    double d = -1.0;
    EXPECT_FALSE(src.tryGetNumber(n, d)) << n;
    EXPECT_EQ(d, -1.0) << "out param must be untouched on a miss";
  }
  EXPECT_EQ(src.fieldCountHint(), 3u);
  EXPECT_TRUE(src.isObject());
}
