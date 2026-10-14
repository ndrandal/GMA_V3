// UNTRACKED ADVERSARIAL PROBE (ENC-1486 review). Delete after use.
#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/MarketTA.hpp"
#include "gma/StreamValue.hpp"
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

#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace gma;

namespace zz {

std::string dd(double d) { char b[40]; std::snprintf(b, sizeof b, "%.17g", d); return b; }

std::string desc(const ArgType& v) {
  return std::visit([](auto&& x) -> std::string {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, double>) return "d:" + dd(x);
    else if constexpr (std::is_same_v<T, int>) return "i:" + std::to_string(x);
    else if constexpr (std::is_same_v<T, bool>) return x ? "b:1" : "b:0";
    else if constexpr (std::is_same_v<T, std::string>) return "s:" + x;
    else return "other";
  }, v);
}

class Log {
public:
  void add(const std::string& l, const StreamValue& sv) {
    std::lock_guard<std::mutex> lk(mx_);
    rows_.push_back(l + "|" + sv.symbol + "|" + desc(sv.value));
  }
  std::vector<std::string> rows() const { std::lock_guard<std::mutex> lk(mx_); return rows_; }
private:
  mutable std::mutex mx_;
  std::vector<std::string> rows_;
};

class Rec final : public INode {
public:
  Rec(std::string l, Log* g) : l_(std::move(l)), g_(g) {}
  void onValue(const StreamValue& sv) override { g_->add(l_, sv); }
  void shutdown() noexcept override {}
private:
  std::string l_; Log* g_;
};

constexpr char kT[] = "PARITY";

const std::vector<std::string>& listened() {
  static const std::vector<std::string> f = {
      "ask","bid","price","size","bidSize","askSize","mean","sum","stddev","lastPrice","sma_5"};
  return f;
}

struct Rig {
  explicit Rig(const util::Config& cfg) : pool(1), disp(&pool, &store, cfg) {
    for (const auto& f : listened()) {
      auto n = std::make_shared<Rec>(f, &log);
      nodes.push_back(n);
      disp.registerListener(kT, f, n);
    }
    symbols.intern("ZZ-PAD");
    id = symbols.intern(kT);
    disp.setSymbolTable(&symbols);
  }
  void quiesce() { pool.drain(); }
  rt::ThreadPool pool; AtomicStore store; data::SymbolTable symbols;
  data::SymbolId id = 0; Dispatcher disp; Log log;
  std::vector<std::shared_ptr<INode>> nodes;
};

data::TickRecord mk(data::SymbolId s, double px, std::uint64_t sz, double bid, double ask) {
  data::TickRecord r;
  r.header.symbolId = s;
  r.kind = data::RecordKind::Trade;
  r.price = data::toFixed(px);
  r.size = sz;
  r.bidPx = data::toFixed(bid);
  r.askPx = data::toFixed(ask);
  r.bidSz = 11; r.askSz = 13;
  return r;
}

// Equivalent event, members added in the mapping order (as the shipped test does).
Event evForward(const std::string& sym, const data::TickRecord& rec) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  struct Add final : engine::NumericFieldSink {
    rapidjson::Document* d;
    void field(const std::string& n, double v) override {
      auto& a = d->GetAllocator();
      d->AddMember(rapidjson::Value(n.c_str(), (rapidjson::SizeType)n.size(), a),
                   rapidjson::Value(v), a);
    }
  } add; add.d = doc.get();
  engine::RecordFieldSource(rec).forEachNumber(add);
  return Event{sym, std::move(doc)};
}

// Same field NAMES and VALUES, reverse member order. Semantically the same tick.
Event evReverse(const std::string& sym, const data::TickRecord& rec) {
  std::vector<std::pair<std::string,double>> fs;
  struct C final : engine::NumericFieldSink {
    std::vector<std::pair<std::string,double>>* o;
    void field(const std::string& n, double v) override { o->emplace_back(n, v); }
  } c; c.o = &fs;
  engine::RecordFieldSource(rec).forEachNumber(c);
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  for (auto it = fs.rbegin(); it != fs.rend(); ++it) {
    auto& a = doc->GetAllocator();
    doc->AddMember(rapidjson::Value(it->first.c_str(), (rapidjson::SizeType)it->first.size(), a),
                   rapidjson::Value(it->second), a);
  }
  return Event{sym, std::move(doc)};
}

std::set<std::string> keys() {
  std::set<std::string> k = {"price","size","bid","ask","bidSize","askSize",
                             "spread","timestamp","lastPrice","volume",
                             "openPrice","highPrice","lowPrice"};
  FunctionMap::instance().forEach([&](const std::string& fn, const Func&) {
    k.insert(fn);
    for (const char* f : {"price","size","bid","ask","bidSize","askSize"})
      k.insert(std::string(f) + "." + fn);
  });
  return k;
}

std::vector<std::string> dump(const AtomicStore& s, const char* sym = kT) {
  std::vector<std::string> o;
  for (const auto& k : keys()) {
    auto v = s.get(sym, k);
    if (v) o.push_back(k + " = " + desc(*v));
  }
  return o;
}

} // namespace zz

// ---------------------------------------------------------------------------
// P1. Does the maxFieldsPerSymbol admission cap make FIELD ORDER observable
//     beyond the setBatch order RecordFieldSource.hpp claims is "the one place"?
// ---------------------------------------------------------------------------
TEST(ZZProbe, AdmissionCapMakesEmissionOrderObservable) {
  util::Config cfg;
  cfg.threadPoolSize = 1;
  cfg.maxFieldsPerSymbol = 3;   // < the 6 fields the mapping can emit

  auto run = [&](int which) {
    zz::Rig rig(cfg);
    auto r = zz::mk(rig.id, 100.25, 10, 100.20, 100.30);
    if (which == 0)      rig.disp.onRecord(r);
    else if (which == 1) rig.disp.onTick(zz::evForward(zz::kT, r));
    else                 rig.disp.onTick(zz::evReverse(zz::kT, r));
    rig.quiesce();
    return zz::dump(rig.store);
  };

  const auto rec  = run(0);
  const auto fwd  = run(1);
  const auto rev  = run(2);

  for (const auto& l : rec) std::printf("REC %s\n", l.c_str());
  std::printf("----\n");
  for (const auto& l : rev) std::printf("REV %s\n", l.c_str());

  EXPECT_EQ(rec, fwd) << "mapping-order JSON must match the record path";
  // If this EXPECT_NE holds, field ORDER is observable through the admission
  // cap, i.e. a graph CAN tell the paths apart for a semantically identical
  // tick whose JSON member order differs from the mapping order.
  EXPECT_NE(rec, rev) << "cap did not make order observable";
}

// ---------------------------------------------------------------------------
// P2. Parity under atomicKeyNamespaceByField = true (the test never sets it).
// ---------------------------------------------------------------------------
TEST(ZZProbe, ParityUnderNamespacedAtomicKeys) {
  util::Config cfg;
  cfg.threadPoolSize = 1;
  cfg.atomicKeyNamespaceByField = true;

  auto run = [&](bool recordPath) {
    zz::Rig rig(cfg);
    for (int i = 0; i < 4; ++i) {
      auto r = zz::mk(rig.id, 100.0 + i * 0.25, 10 + i, 99.0 + i, 101.0 + i);
      if (recordPath) rig.disp.onRecord(r);
      else            rig.disp.onTick(zz::evForward(zz::kT, r));
    }
    rig.quiesce();
    return std::make_pair(rig.log.rows(), zz::dump(rig.store));
  };
  auto [dj, sj] = run(false);
  auto [dr, sr] = run(true);
  ASSERT_FALSE(dj.empty());
  EXPECT_EQ(dj, dr);
  EXPECT_EQ(sj, sr);
}

// ---------------------------------------------------------------------------
// P3. A non-object JSON payload with a live subscription. Pre-change this
//     reached rapidjson's HasMember on a non-object (assert / UB).
// ---------------------------------------------------------------------------
TEST(ZZProbe, NonObjectPayloadDoesNotCrashNow) {
  util::Config cfg; cfg.threadPoolSize = 1;
  zz::Rig rig(cfg);
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetArray();
  rig.disp.onTick(Event{zz::kT, doc});
  auto doc2 = std::make_shared<rapidjson::Document>();
  doc2->SetInt(5);
  rig.disp.onTick(Event{zz::kT, doc2});
  rig.quiesce();
  EXPECT_TRUE(rig.log.rows().empty());
  EXPECT_TRUE(zz::dump(rig.store).empty());
}

// ---------------------------------------------------------------------------
// P4. Duplicate JSON member names — raw admission and the listener read.
// ---------------------------------------------------------------------------
TEST(ZZProbe, DuplicateJsonMembers) {
  util::Config cfg; cfg.threadPoolSize = 1;
  zz::Rig rig(cfg);
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  auto& a = doc->GetAllocator();
  doc->AddMember("price", rapidjson::Value(1.0), a);
  doc->AddMember("price", rapidjson::Value(2.0), a);
  rig.disp.onTick(Event{zz::kT, doc});
  rig.quiesce();
  for (const auto& r : rig.log.rows()) std::printf("DUP delivery %s\n", r.c_str());
  auto v = rig.store.get(zz::kT, "price");
  ASSERT_TRUE(v.has_value());
  std::printf("DUP store price = %s\n", zz::desc(*v).c_str());
}

// ---------------------------------------------------------------------------
// P5. Can a zero-field record burn a _rawAdmitted symbol slot?
// ---------------------------------------------------------------------------
TEST(ZZProbe, ZeroFieldRecordBurnsASymbolSlot) {
  util::Config cfg; cfg.threadPoolSize = 1; cfg.maxSymbols = 1;
  // Record path: an empty record for symbol A, then a real one for PARITY.
  {
    zz::Rig rig(cfg);
    const auto other = rig.symbols.intern("OTHER");
    data::TickRecord empty;           // all kNoPrice, size 0 -> emits NOTHING
    empty.header.symbolId = other;
    rig.disp.onRecord(empty);
    rig.disp.onRecord(zz::mk(rig.id, 100.25, 10, 100.20, 100.30));
    rig.quiesce();
    const auto d = zz::dump(rig.store);
    std::printf("P5 record-path store keys for PARITY: %zu\n", d.size());
    EXPECT_FALSE(rig.store.get(zz::kT, "price").has_value())
        << "the empty record did NOT burn the only _rawAdmitted slot";
  }
  // JSON path with a NON-OBJECT payload for A does not burn the slot.
  {
    zz::Rig rig(cfg);
    auto nonobj = std::make_shared<rapidjson::Document>();
    nonobj->SetArray();
    rig.disp.onTick(Event{"OTHER", nonobj});
    rig.disp.onTick(zz::evForward(zz::kT, zz::mk(rig.id, 100.25, 10, 100.20, 100.30)));
    rig.quiesce();
    std::printf("P5 json(non-object first) PARITY price present = %d\n",
                (int)rig.store.get(zz::kT, "price").has_value());
  }
}

// ---------------------------------------------------------------------------
// P6. kMaxFields: exactly 6, and a non-Trade kind still emits the quote half.
// ---------------------------------------------------------------------------
TEST(ZZProbe, MaxFieldsAndNonTradeKinds) {
  data::TickRecord r;
  r.kind = data::RecordKind::Trade;
  r.price = 1; r.size = 1; r.bidPx = 1; r.askPx = 1; r.bidSz = 1; r.askSz = 1;
  EXPECT_EQ(engine::RecordFieldSource(r).fieldCountHint(),
            engine::RecordFieldSource::kMaxFields);

  for (auto k : {data::RecordKind::Quote, data::RecordKind::BookEvent,
                 data::RecordKind::BookSnapshot}) {
    data::TickRecord q; q.kind = k;
    q.bidPx = data::toFixed(1.0); q.askPx = data::toFixed(2.0);
    q.bidSz = 5; q.askSz = 6;
    struct D final : engine::NumericFieldSink {
      std::string o;
      void field(const std::string& n, double v) override { o += n + "=" + zz::dd(v) + ";"; }
    } d;
    engine::RecordFieldSource(q).forEachNumber(d);
    std::printf("P6 kind=%d -> %s\n", (int)k, d.o.c_str());
  }
}

// ---------------------------------------------------------------------------
// P7. The timestamp channel: JSON has one, records do not.
// ---------------------------------------------------------------------------
TEST(ZZProbe, JsonHasATimestampChannelRecordsDoNot) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  auto& a = doc->GetAllocator();
  doc->AddMember("price", rapidjson::Value(100.0), a);
  doc->AddMember("ts", rapidjson::Value((uint64_t)1700000000000000000ULL), a);
  const engine::JsonFieldSource js(doc.get());
  std::uint64_t t = 0;
  EXPECT_TRUE(js.tryGetUint64("ts", t));
  EXPECT_EQ(t, 1700000000000000000ULL);

  data::TickRecord r;
  r.kind = data::RecordKind::Trade;
  r.price = data::toFixed(100.0);
  r.header.tsEventNs = 1700000000000000000LL;
  const engine::RecordFieldSource rs(r);
  std::uint64_t t2 = 0;
  EXPECT_FALSE(rs.tryGetUint64("ts", t2));
  EXPECT_FALSE(rs.has("ts"));
  EXPECT_FALSE(rs.has("timestamp"));
}

// ---------------------------------------------------------------------------
// P8. Huge uint64 size: lossy double on both paths, identically?
// ---------------------------------------------------------------------------
TEST(ZZProbe, HugeSizesRoundTripIdentically) {
  data::TickRecord r;
  r.kind = data::RecordKind::Trade;
  r.price = data::toFixed(1.0);
  r.size  = 18446744073709551615ULL;
  r.bidPx = data::toFixed(1.0); r.bidSz = 9007199254740993ULL;
  const engine::RecordFieldSource rs(r);
  const Event ev = zz::evForward("X", r);
  const engine::JsonFieldSource js(ev.payload.get());
  for (const char* n : {"price","size","bid","bidSize"}) {
    double a = 0, b = 0;
    ASSERT_TRUE(rs.tryGetNumber(n, a)) << n;
    ASSERT_TRUE(js.tryGetNumber(n, b)) << n;
    EXPECT_EQ(zz::dd(a), zz::dd(b)) << n;
  }
}

// ---------------------------------------------------------------------------
// P9. Negative / zero-price records: kNoPrice sentinel edges.
// ---------------------------------------------------------------------------
TEST(ZZProbe, PriceSentinelEdges) {
  // price == 0 is a LEGAL price and must be emitted.
  data::TickRecord z; z.kind = data::RecordKind::Trade; z.price = 0;
  EXPECT_TRUE(engine::RecordFieldSource(z).has("price"));
  // INT64_MIN+1 is not the sentinel, so it is emitted and decodes to ~-9.2e9.
  data::TickRecord m; m.kind = data::RecordKind::Trade; m.price = INT64_MIN + 1;
  double d = 0;
  ASSERT_TRUE(engine::RecordFieldSource(m).tryGetNumber("price", d));
  std::printf("P9 INT64_MIN+1 -> %s\n", zz::dd(d).c_str());
}
