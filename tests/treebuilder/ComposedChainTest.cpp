// ENC-1290 — `node` and `pipeline` compose into ONE chain, and the outer
// `Listener(streamKey, field)` is that chain's CLOCK.
//
// SPEC specs/2026-09-20-gma-join-correctness D5 and §5 Q1 (ruled 2026-09-20 by
// ENC-1317; the rule's first wording was broken by that ticket's own
// adversarial review and replaced — Corrections C4.0).
//
// ─────────────────────────────────────────────────────────────────────────────
// THE SHAPE THIS FILE GATES
//
//     Listener(streamKey, field) -> node subtree -> pipeline stages -> terminal
//
// and nothing reaches the terminal except through the pipeline. Before this,
// `buildForRequest` wired the `node` subtree straight to the terminal and then
// restarted from the terminal for the pipeline, leaving BOTH chains live on one
// Responder: the client got two unrelated streams interleaved on one request
// key and the pipeline never saw the values it exists to reduce (SPEC §1.1
// defect 1). 52 of the 272 checked-in corpus requests carry both keys.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE PART THAT IS EASY TO GET BACKWARDS, AND WHICH ONE TEST HERE EXISTS FOR
//
// The head Listener is ALWAYS built. For a FAN-IN node it is not a member of
// the join — the members are the node's own declared `inputs` — so its value
// must never enter `Aggregate::buf_`. `CompositeRoot` forwards it to an
// explicit `clockTargets` set and to nothing else, and that set is filled from
// DECLARED INPUT HEADS ONLY, and only those with no `Listener` in their
// subtree.
//
// The rule's first published wording — "forward to the roots that are not
// themselves Dispatcher-subscribed" — reads plausibly and is WRONG, because
// `roots_` is a lifecycle list that contains the `Aggregate` itself. Under it
// the clock's `lastPrice` lands in `buf_[sv.symbol]` as a join member, which is
// SPEC §1.1 defect 2 manufactured inside the builder.
// `ClockIsNeverAJoinMember` below fails under that implementation and passes
// under this one. It is the only reason that distinction is checkable.
//
// ─────────────────────────────────────────────────────────────────────────────
// SCOPE — FAN-IN NODES ONLY
//
// For a single-input transform node (Worker, AtomicAccessor, Interval,
// GroupSplit) the outer Listener IS the data source, before and after. All 162
// `node`-only corpus requests are in that category and NONE of them builds a
// `CompositeRoot`. `NodeOnlyCorpusRequestsAreUnaffected` proves the scope
// clause holds across all 162 rather than asserting it in prose.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT THIS FILE DOES **NOT** CLAIM
//
// That corpus 86 now reports the 0.02 spread. It does not, and D5 was never
// going to make it — see `Corpus86ComposedChainEmitsTheExactMeasuredValues`,
// whose pinned values are the DEFECT, recorded so the next ticket inherits a
// measurement instead of a guess (SPEC §5 Q5, open; ENC-1291/ENC-1005 own it).

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace gma;

// ─── Recording terminal ─────────────────────────────────────────────────────
class Sink final : public INode {
public:
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    if (const double* d = std::get_if<double>(&sv.value)) vals_.push_back(*d);
    else                                                  ++nonNumeric_;
  }
  void shutdown() noexcept override {}

  std::vector<double> values() const {
    std::lock_guard<std::mutex> lk(mx_); return vals_;
  }
  std::size_t nonNumeric() const {
    std::lock_guard<std::mutex> lk(mx_); return nonNumeric_;
  }

private:
  mutable std::mutex  mx_;
  std::vector<double> vals_;
  std::size_t         nonNumeric_{0};
};

std::string render(const std::vector<double>& v) {
  std::string s = "[";
  for (std::size_t i = 0; i < v.size(); ++i) {
    if (i) s += ", ";
    s += std::to_string(v[i]);
  }
  return s + "]";
}

std::string toJson(const rapidjson::Value& v) {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  v.Accept(w);
  return sb.GetString();
}

// ─── Corpus access ──────────────────────────────────────────────────────────
// Same search paths as CorpusTest / RecordTerminalTest: CMake copies the
// corpus next to the binary. A missing corpus must be LOUD, never a skip
// (ENC-807 L17), so every caller ASSERTs on this.
rapidjson::Document& corpusDoc() {
  static rapidjson::Document doc = [] {
    const char* paths[] = {
      "corpus_requests.json",
      "../tests/treebuilder/corpus_requests.json",
      "tests/treebuilder/corpus_requests.json",
    };
    rapidjson::Document d;
    for (const char* p : paths) {
      std::ifstream ifs(p);
      if (!ifs.is_open()) continue;
      rapidjson::IStreamWrapper isw(ifs);
      d.ParseStream(isw);
      return d;
    }
    d.SetNull();
    return d;
  }();
  return doc;
}

const rapidjson::Value* corpusRequest(int corpusId, int key = 1) {
  rapidjson::Document& d = corpusDoc();
  if (d.IsNull() || d.HasParseError() || !d.IsArray()) return nullptr;
  for (auto& e : d.GetArray()) {
    if (!e.IsObject() || !e.HasMember("corpus_id") || !e["corpus_id"].IsInt()) continue;
    if (e["corpus_id"].GetInt() != corpusId) continue;
    if (!e.HasMember("request")) continue;
    const auto& rq = e["request"];
    const int k = (rq.HasMember("key") && rq["key"].IsInt()) ? rq["key"].GetInt() : 1;
    if (k != key) continue;
    return &rq;
  }
  return nullptr;
}

bool hasNode(const rapidjson::Value& rq) {
  return rq.HasMember("node") && rq["node"].IsObject();
}
bool hasPipeline(const rapidjson::Value& rq) {
  for (const char* k : {"pipeline", "stages"})
    if (rq.HasMember(k) && rq[k].IsArray()) return true;
  return false;
}

// ─── Fixture ────────────────────────────────────────────────────────────────
// One worker thread throughout. Nothing in this file is a race test, and a
// single FIFO worker makes `Dispatcher::onTick`'s notification order
// deterministic (the per-symbol listener container is a std::map, so field
// names are notified in lexicographic order: "ask" before "bid" before
// "lastPrice").
class ComposedChain : public ::testing::Test {
protected:
  void SetUp() override {
    prevPool_   = gThreadPool;
    pool_       = std::make_shared<rt::ThreadPool>(1);
    gThreadPool = pool_;
    dispatcher_ = std::make_unique<Dispatcher>(pool_.get(), &store_);
    deps_.store      = &store_;
    deps_.pool       = pool_.get();
    deps_.dispatcher = dispatcher_.get();
  }

  void TearDown() override {
    dispatcher_.reset();
    if (pool_) pool_->shutdown();
    pool_.reset();
    gThreadPool = prevPool_;
  }

  void tick(const char* symbol,
            std::initializer_list<std::pair<const char*, double>> fields) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& al = payload->GetAllocator();
    for (const auto& [name, value] : fields)
      payload->AddMember(rapidjson::StringRef(name), value, al);
    Event ev;
    ev.symbol  = symbol;          // ev.type defaults to "tick" — production type
    ev.payload = payload;
    dispatcher_->onTick(ev);
  }

  void tick(const std::string& symbol,
            const std::vector<std::pair<std::string, double>>& fields) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& al = payload->GetAllocator();
    for (const auto& [name, value] : fields)
      payload->AddMember(rapidjson::Value(name.c_str(), al).Move(),
                         rapidjson::Value(value).Move(), al);
    Event ev;
    ev.symbol  = symbol;
    ev.payload = payload;
    dispatcher_->onTick(ev);
  }

  AtomicStore                     store_;
  std::shared_ptr<rt::ThreadPool> pool_;
  std::shared_ptr<rt::ThreadPool> prevPool_;
  std::unique_ptr<Dispatcher>     dispatcher_;
  tree::Deps                      deps_;
};

// ═══════════════════════════════════════════════════════════════════════════
// 1. THE COMPOSITION ITSELF — corpus 86, end to end, values pinned
// ═══════════════════════════════════════════════════════════════════════════
//
// corpus_id 86 "Bid-ask spread for AAPL", verbatim:
//   {streamKey:AAPL, field:lastPrice,
//    node: Aggregate{arity:2, inputs:[Listener(AAPL,ask), Listener(AAPL,bid)]},
//    pipeline: [Worker{fn:"diff"}]}
//
// BEFORE: the `Aggregate` fed the terminal directly, so ticking bid/ask put the
// RAW PRICES (1000.02, 1000.00, …) on the wire and `Worker{fn:"diff"}` — on its
// own dead chain behind Listener(AAPL,lastPrice) — never ran.
//
// AFTER: every value reaches the terminal THROUGH the Worker. No raw price can
// appear at all, which is the structural half of the assertion; the exact
// sequence is the other half.
//
// THE PINNED VALUES ARE THE DEFECT, NOT THE ANSWER. The truth on every tick is
// a +0.02 spread; the chain emits 0.00, -0.02, 1.00, 0.98, 2.00, 1.98. Three
// lines cause it and none of them is D5's to fix:
//   * `Aggregate::onValue` forwards a completed batch MEMBER BY MEMBER
//     (src/nodes/Aggregate.cpp), so the Worker sees two separate values, never
//     a pair;
//   * `Worker::onValue` runs its fn over a per-symbol accumulator cleared only
//     in shutdown() (src/nodes/Worker.cpp), so it never forgets tick 0;
//   * `diff` is `v.back() - v.front()` (src/core/BuiltinFunctions.cpp).
// That is SPEC §5 Q5 — filed by ENC-1317, still OPEN. Pinning the measurement
// here is deliberate: the next ticket inherits a number it can diff against
// rather than a prediction. WHEN Q5 IS RULED AND FIXED, this expectation
// changes and this comment comes with it.
TEST_F(ComposedChain, Corpus86ComposedChainEmitsTheExactMeasuredValues) {
  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";
  ASSERT_TRUE(hasNode(*req) && hasPipeline(*req))
      << "corpus_id 86 must carry BOTH keys for this test to mean anything: "
      << toJson(*req);

  auto sink = std::make_shared<Sink>();
  tree::BuiltChain chain;
  ASSERT_NO_THROW(chain = tree::buildForRequest(*req, deps_, sink));
  ASSERT_NE(chain.head, nullptr);

  constexpr double kBase = 1000.00, kSpread = 0.02;
  for (int n = 0; n < 3; ++n)
    tick("AAPL", {{"bid", kBase + n}, {"ask", kBase + n + kSpread}});
  pool_->drain();

  const auto vals = sink->values();
  EXPECT_EQ(sink->nonNumeric(), 0u);

  // Structural: nothing may reach the terminal except through the pipeline.
  // A raw price is two orders of magnitude above any difference of two of
  // them, so "did a raw price arrive?" is decidable by value alone.
  for (double v : vals)
    EXPECT_LT(v, 900.0)
        << "a RAW input value (" << v << ") reached the terminal. The `node` "
           "subtree is still wired around the pipeline instead of through it — "
           "SPEC §1.1 defect 1. Got " << render(vals);

  const std::vector<double> kExpected = {0.00, -0.02, 1.00, 0.98, 2.00, 1.98};
  ASSERT_EQ(vals.size(), kExpected.size())
      << "expected six arrivals (three ticks x a two-member batch forwarded "
         "one at a time), got " << render(vals);
  for (std::size_t i = 0; i < kExpected.size(); ++i)
    EXPECT_NEAR(vals[i], kExpected[i], 1e-9)
        << "arrival " << i << " of " << render(vals)
        << "\n    These values are WRONG — the spread is 0.02 on every tick — "
           "and they are pinned\n    because SPEC §5 Q5 is open, not because "
           "they are correct. See this test's comment.";

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  chain.head->shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// 2. THE CLOCK RULE — the head Listener must never become a join member
// ═══════════════════════════════════════════════════════════════════════════
//
// Same request, same ticks, but each tick ALSO carries `lastPrice` — the outer
// Listener's own field — with a value no bid or ask ever takes. Because both of
// the Aggregate's declared inputs carry Listeners, the clock's forwarding set
// is EMPTY and the head Listener stays inert: the emitted sequence must be
// identical to the test above, to the last bit.
//
// THIS IS THE REFUTATION TEST. Implement the rule's retracted wording —
// "CompositeRoot forwards to the roots that are not themselves
// Dispatcher-subscribed" — and `roots_` hands you the `Aggregate` itself, so
// 7777 is pushed into `buf_["AAPL"]` as a join member, every batch shifts, and
// this test fails. Delete the `clockTargets` filter entirely and it fails the
// same way. There is no other test in the suite that separates the two rules.
TEST_F(ComposedChain, ClockIsNeverAJoinMember) {
  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr);

  auto sink = std::make_shared<Sink>();
  tree::BuiltChain chain;
  ASSERT_NO_THROW(chain = tree::buildForRequest(*req, deps_, sink));

  constexpr double kBase = 1000.00, kSpread = 0.02, kClock = 7777.0;
  for (int n = 0; n < 3; ++n)
    tick("AAPL", {{"bid", kBase + n},
                  {"ask", kBase + n + kSpread},
                  {"lastPrice", kClock}});   // the CLOCK's own field
  pool_->drain();

  const std::vector<double> kExpected = {0.00, -0.02, 1.00, 0.98, 2.00, 1.98};
  const auto vals = sink->values();
  EXPECT_EQ(sink->nonNumeric(), 0u);

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  if (chain.head) chain.head->shutdown();

  ASSERT_EQ(vals.size(), kExpected.size())
      << "driving the outer Listener's own field changed the ARRIVAL COUNT: "
      << render(vals) << " vs the clock-silent run "
      << render(kExpected)
      << "\n    The head Listener(AAPL,lastPrice) is the chain's CLOCK. Both of "
         "corpus 86's declared\n    inputs carry Listeners of their own, so the "
         "clock's forwarding set is EMPTY and\n    `lastPrice` must contribute "
         "NOTHING.\n"
         "    If this fails, the clock is being forwarded to something that is "
         "not a declared\n    input head — almost certainly `roots_`, which "
         "holds the Aggregate ITSELF. That is\n    SPEC §1.1 defect 2 "
         "manufactured inside the builder (ENC-1317 Corrections C4.0).";
  for (std::size_t i = 0; i < kExpected.size(); ++i)
    EXPECT_NEAR(vals[i], kExpected[i], 1e-9)
        << "arrival " << i << ": the clock value " << kClock
        << " has entered the join. Got " << render(vals);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3. THE SEVEN PULL-ONLY JOINS — dead today, clocked from now on
// ═══════════════════════════════════════════════════════════════════════════
//
// corpus_id 111 "Bollinger bandwidth for AAPL — upper minus lower band":
//   node: Aggregate{arity:2, inputs:[AtomicAccessor(AAPL,bollinger_upper),
//                                    AtomicAccessor(AAPL,bollinger_lower)]}
//   pipeline: [Worker{fn:"diff"}]
//
// An `AtomicAccessor` is a PULL node: it has no Dispatcher subscription and
// only samples the store when something calls its `onValue`. Nothing ever did.
// So this entry — and ids 112, 113, 114, 115, 192, 200 with it — was not "two
// live chains interleaved" (SPEC §1.1 defect 1's stated symptom, corrected by
// ENC-1317's C2.1); it was ONE live chain and a join that never fired once.
// The worse failure mode: a plausible number instead of a visibly wrong one.
//
// With the head Listener as its clock the join fires for the first time.
TEST_F(ComposedChain, Corpus111PullOnlyJoinFiresForTheFirstTime) {
  const rapidjson::Value* req = corpusRequest(111);
  ASSERT_NE(req, nullptr) << "corpus_id 111 not found in corpus_requests.json";

  store_.set("AAPL", "bollinger_upper", 110.0);
  store_.set("AAPL", "bollinger_lower", 90.0);

  auto sink = std::make_shared<Sink>();
  tree::BuiltChain chain;
  ASSERT_NO_THROW(chain = tree::buildForRequest(*req, deps_, sink));

  tick("AAPL", {{"lastPrice", 123.45}});     // one clock tick, nothing else
  pool_->drain();

  const auto vals = sink->values();
  ASSERT_FALSE(vals.empty())
      << "corpus_id 111's join emitted NOTHING. Both of its declared inputs are "
         "AtomicAccessors —\n    pull nodes with no Dispatcher subscription — so "
         "without the head Listener clocking them\n    this request is inert, "
         "which is what it was before ENC-1290 (SPEC §5 Q1, probe S2).";

  // One clock tick samples both accessors under symbol "AAPL", completing one
  // arity-2 batch. `Aggregate` forwards it member by member into
  // `Worker{fn:"diff"}` (= back - front over a never-cleared accumulator), so:
  //   upper 110 arrives -> acc [110]      -> 110 - 110 =   0
  //   lower  90 arrives -> acc [110, 90]  ->  90 - 110 = -20
  const std::vector<double> kExpected = {0.0, -20.0};
  ASSERT_EQ(vals.size(), kExpected.size()) << render(vals);
  for (std::size_t i = 0; i < kExpected.size(); ++i)
    EXPECT_NEAR(vals[i], kExpected[i], 1e-9) << "arrival " << i << " of " << render(vals);

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  if (chain.head) chain.head->shutdown();
}

// ═══════════════════════════════════════════════════════════════════════════
// 4. THE STRUCTURAL SWEEP — all 52, and the ONE path to the terminal
// ═══════════════════════════════════════════════════════════════════════════
//
// The generic statement of D5 is "nothing reaches the terminal except through
// the pipeline", and it is checkable without knowing a single entry's
// semantics: append an always-false `Filter` to the pipeline and the terminal
// must go completely silent. Under the old two-chain wiring the `node` subtree
// bypassed the pipeline entirely, so it kept emitting straight past the filter.
//
// Two runs per entry, and BOTH assertions matter:
//   A. verbatim              -> at least one arrival   (the driver really
//                               exercises this entry; without this, B passes
//                               vacuously for a chain that was simply dead)
//   B. + always-false Filter -> exactly zero arrivals  (the only path to the
//                               terminal runs through the pipeline)
TEST_F(ComposedChain, EveryNodePlusPipelineEntryReachesTheTerminalOnlyViaThePipeline) {
  rapidjson::Document& doc = corpusDoc();
  ASSERT_FALSE(doc.IsNull()) << "corpus_requests.json not found next to the test binary";
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());

  int seen = 0, silentA = 0, leakedB = 0;
  std::string detail;

  for (const auto& entry : doc.GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("request")) continue;
    const rapidjson::Value& rq = entry["request"];
    if (!hasNode(rq) || !hasPipeline(rq)) continue;
    ++seen;
    const int id = (entry.HasMember("corpus_id") && entry["corpus_id"].IsInt())
                     ? entry["corpus_id"].GetInt() : -1;

    // Everything the request mentions, so the driver can actually drive it:
    // every AtomicAccessor's field is seeded in the store, and every
    // (streamKey, field) a Listener names — plus the outer pair — is ticked.
    std::map<std::string, std::set<std::string>> tickFields;
    double seedVal = 10.0;
    std::function<void(const rapidjson::Value&)> walk =
      [&](const rapidjson::Value& v) {
        if (v.IsArray()) { for (const auto& x : v.GetArray()) walk(x); return; }
        if (!v.IsObject()) return;
        if (v.HasMember("type") && v["type"].IsString()) {
          const std::string t = v["type"].GetString();
          const std::string sk =
            (v.HasMember("streamKey") && v["streamKey"].IsString())
              ? v["streamKey"].GetString()
              : (rq["streamKey"].IsString() ? rq["streamKey"].GetString() : "");
          const std::string f = (v.HasMember("field") && v["field"].IsString())
                                  ? v["field"].GetString() : "";
          if (t == "AtomicAccessor" && !f.empty())
            store_.set(sk, f, seedVal += 1.0);
          if (t == "Listener" && !f.empty()) tickFields[sk].insert(f);
        }
        for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) walk(m->value);
      };
    walk(rq["node"]);
    tickFields[rq["streamKey"].GetString()].insert(rq["field"].GetString());

    auto drive = [&](const rapidjson::Value& request) -> std::size_t {
      auto sink = std::make_shared<Sink>();
      tree::BuiltChain chain;
      try {
        chain = tree::buildForRequest(request, deps_, sink);
      } catch (const std::exception& ex) {
        ADD_FAILURE() << "corpus_id " << id << " failed to build: " << ex.what();
        return 0;
      }
      double v = 100.0;
      for (int rep = 0; rep < 4; ++rep)
        for (const auto& [sk, fields] : tickFields) {
          std::vector<std::pair<std::string, double>> kv;
          for (const auto& f : fields) kv.emplace_back(f, v += 1.0);
          tick(sk, kv);
        }
      pool_->drain();
      const std::size_t n = sink->values().size();
      for (auto& x : chain.keepAlive) if (x) x->shutdown();
      if (chain.head) chain.head->shutdown();
      return n;
    };

    const std::size_t a = drive(rq);

    // B: the same request with `{"type":"Filter","when":{"op":"lt","args":[1,0]}}`
    // appended as the LAST pipeline stage. `Filter` passes a value on only when
    // its predicate is > 0.5 (src/nodes/Filter.cpp) and `1 < 0` compiles to a
    // constant 0.0, so the composed tail is a wall.
    rapidjson::Document blocked;
    blocked.CopyFrom(rq, blocked.GetAllocator());
    auto& al = blocked.GetAllocator();
    const char* key = blocked.HasMember("pipeline") ? "pipeline" : "stages";
    rapidjson::Value wall(rapidjson::kObjectType);
    rapidjson::Value when(rapidjson::kObjectType);
    rapidjson::Value args(rapidjson::kArrayType);
    args.PushBack(1, al).PushBack(0, al);
    when.AddMember("op", "lt", al).AddMember("args", args, al);
    wall.AddMember("type", "Filter", al).AddMember("when", when, al);
    blocked[key].PushBack(wall, al);

    const std::size_t b = drive(blocked);

    if (a == 0) {
      ++silentA;
      detail += "\n      corpus_id " + std::to_string(id) +
                ": verbatim run emitted nothing — B proves nothing for it";
    }
    if (b != 0) {
      ++leakedB;
      detail += "\n      corpus_id " + std::to_string(id) + ": " +
                std::to_string(b) +
                " value(s) reached the terminal past an always-false Filter at "
                "the END of the pipeline";
    }
  }

  EXPECT_EQ(seen, 52)
      << "expected 52 corpus entries carrying BOTH `node` and `pipeline` (SPEC "
         "§5 Q1's classification); found " << seen
      << ". If the corpus changed size, re-run the classification before "
         "trusting anything below.";
  EXPECT_EQ(silentA, 0) << silentA << " entr(ies) emitted nothing at all:" << detail;
  EXPECT_EQ(leakedB, 0)
      << leakedB << " of " << seen << " entries reach the terminal WITHOUT "
         "passing through the pipeline.\n"
         "    That is SPEC §1.1 defect 1: the `node` subtree wired straight to "
         "the Responder while\n"
         "    the pipeline runs on a second, separate chain." << detail;
}

// ═══════════════════════════════════════════════════════════════════════════
// 5. THE SCOPE CLAUSE — the 162 `node`-only requests are untouched
// ═══════════════════════════════════════════════════════════════════════════
//
// The clock rule applies to FAN-IN nodes only. Every one of the 162 `node`-only
// corpus requests has a single-input transform at its head (Worker 47,
// AtomicAccessor 81, Interval 24, SymbolSplit 10 — independently recounted
// here), none of them builds a `CompositeRoot`, and for them the head Listener
// is wired straight into the node and is the data source. Without the scope
// clause the rule would have broken all 162.
//
// ONE CORRECTION TO THE RULING, MEASURED HERE RATHER THAN ARGUED. §5 Q1 says
// "for all 162 the head Listener IS the sole data source". That holds for 138
// of them. It does not hold for the 24 whose head is an `Interval`:
// `Interval::onValue` is an explicit no-op ("source node: no upstream input",
// src/nodes/Interval.cpp) and a timer thread drives the child instead. Those 24
// ignore the head Listener before this change and after it — which is still
// "untouched", just not for the stated reason. The assertion below splits the
// two groups rather than asserting the ruling's wording.
TEST_F(ComposedChain, NodeOnlyCorpusRequestsAreUnaffected) {
  rapidjson::Document& doc = corpusDoc();
  ASSERT_FALSE(doc.IsNull()) << "corpus_requests.json not found next to the test binary";
  ASSERT_TRUE(doc.IsArray());

  int seen = 0, driven = 0, timerDriven = 0, silent = 0;
  std::string detail;

  for (const auto& entry : doc.GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("request")) continue;
    const rapidjson::Value& rq = entry["request"];
    if (!hasNode(rq) || hasPipeline(rq)) continue;
    ++seen;
    const int id = (entry.HasMember("corpus_id") && entry["corpus_id"].IsInt())
                     ? entry["corpus_id"].GetInt() : -1;
    const std::string headType = rq["node"]["type"].GetString();

    // Seed every AtomicAccessor the subtree names, so a pull head has
    // something to return.
    double seedVal = 42.0;
    std::function<void(const rapidjson::Value&)> walk =
      [&](const rapidjson::Value& v) {
        if (v.IsArray()) { for (const auto& x : v.GetArray()) walk(x); return; }
        if (!v.IsObject()) return;
        if (v.HasMember("type") && v["type"].IsString() &&
            std::string(v["type"].GetString()) == "AtomicAccessor" &&
            v.HasMember("field") && v["field"].IsString()) {
          const std::string sk =
            (v.HasMember("streamKey") && v["streamKey"].IsString())
              ? v["streamKey"].GetString() : rq["streamKey"].GetString();
          store_.set(sk, v["field"].GetString(), seedVal += 1.0);
        }
        for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) walk(m->value);
      };
    walk(rq["node"]);

    auto sink = std::make_shared<Sink>();
    tree::BuiltChain chain;
    try {
      chain = tree::buildForRequest(rq, deps_, sink);
    } catch (const std::exception& ex) {
      ADD_FAILURE() << "corpus_id " << id << " failed to build: " << ex.what();
      continue;
    }

    // Drive the OUTER (streamKey, field) and nothing else. For a single-input
    // transform head that is the data source, so a value must come out.
    for (int rep = 0; rep < 3; ++rep)
      tick(rq["streamKey"].GetString(),
           {{rq["field"].GetString(), 100.0 + rep}});
    pool_->drain();

    const std::size_t n = sink->values().size();
    for (auto& x : chain.keepAlive) if (x) x->shutdown();
    if (chain.head) chain.head->shutdown();

    if (headType == "Interval") { ++timerDriven; continue; }
    if (n > 0) { ++driven; continue; }
    ++silent;
    detail += "\n      corpus_id " + std::to_string(id) + " (head " + headType +
              "): ticking the outer (" + rq["streamKey"].GetString() + ", " +
              rq["field"].GetString() + ") produced NOTHING";
  }

  EXPECT_EQ(seen, 162)
      << "expected 162 `node`-only corpus entries (SPEC §5 Q1); found " << seen;
  EXPECT_EQ(timerDriven, 24)
      << "expected 24 Interval-headed entries, which are timer-driven and "
         "correctly ignore the head Listener; found " << timerDriven;
  EXPECT_EQ(silent, 0)
      << silent << " of the " << (seen - timerDriven)
      << " non-Interval `node`-only entries stopped being fed by their head "
         "Listener.\n"
         "    The ENC-1290 clock rule is scoped to FAN-IN nodes; for a "
         "single-input transform the\n"
         "    outer Listener IS the data source and nothing about it may "
         "change." << detail;
  EXPECT_EQ(driven, 138) << "138 = 47 Worker + 81 AtomicAccessor + 10 SymbolSplit";
}

// ═══════════════════════════════════════════════════════════════════════════
// 6. ENC-1293's CHECK, NARROWED — `node:Pack` + `pipeline:[Field]` is LEGAL
// ═══════════════════════════════════════════════════════════════════════════
//
// D7 rejects a Record-valued TERMINAL and blesses `Pack -> Field -> scalar ->
// Responder` one line above its own restriction. ENC-1293's check analysed
// `node` and `pipeline` as two independent feeders, which was right while they
// were two chains — and which rejects exactly that canonical shape the moment
// they become one.
//
// Nothing in the corpus goes red on the day that becomes wrong: all 272 entries
// construct no `Record` at all (Worker, AtomicAccessor, Listener, Aggregate,
// Interval, SymbolSplit only), so `AllCorpusRequestsBuild` and
// `NoCheckedInCorpusRequestIsRefused` would both stay green while the check
// refused something it should accept. This test is the gate instead.
TEST_F(ComposedChain, NodePackPipelineFieldIsAcceptedAndEmits) {
  const char* kRequest = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Pack","fields":{
        "ask":{"type":"Listener","streamKey":"AAPL","field":"ask"},
        "bid":{"type":"Listener","streamKey":"AAPL","field":"bid"}}},
    "pipeline":[{"type":"Field","name":"bid"}]
  })";

  rapidjson::Document d;
  d.Parse(kRequest);
  ASSERT_FALSE(d.HasParseError());

  auto sink = std::make_shared<Sink>();
  tree::BuiltChain chain;
  ASSERT_NO_THROW(chain = tree::buildForRequest(d, deps_, sink))
      << "node:Pack + pipeline:[Field] is the canonical D7 shape and must build. "
         "If this throws the Record-terminal check is still analysing `node` as "
         "a direct feeder of the terminal (ENC-1293's call site), which "
         "ENC-1290 composed away.";
  ASSERT_NE(chain.head, nullptr);

  tick("AAPL", {{"bid", 1000.00}, {"ask", 1000.02}});
  pool_->drain();

  const auto vals = sink->values();
  EXPECT_EQ(sink->nonNumeric(), 0u)
      << "a Record reached the terminal — the rejection is not doing its job";
  ASSERT_EQ(vals.size(), 1u) << "expected exactly one projected value: " << render(vals);
  EXPECT_DOUBLE_EQ(vals[0], 1000.00) << "Field{name:'bid'} must project the bid";

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  chain.head->shutdown();
}

// The other direction, so §6 cannot be read as "the check was switched off":
// with NO pipeline, the `node` subtree IS what feeds the terminal and a Pack
// there is still refused.
TEST_F(ComposedChain, NodePackWithNoPipelineIsStillRejected) {
  const char* kRequest = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Pack","fields":{
        "ask":{"type":"Listener","streamKey":"AAPL","field":"ask"},
        "bid":{"type":"Listener","streamKey":"AAPL","field":"bid"}}}
  })";
  rapidjson::Document d;
  d.Parse(kRequest);
  ASSERT_FALSE(d.HasParseError());
  auto sink = std::make_shared<Sink>();
  try {
    auto chain = tree::buildForRequest(d, deps_, sink);
    for (auto& n : chain.keepAlive) if (n) n->shutdown();
    if (chain.head) chain.head->shutdown();
    FAIL() << "node:Pack with no pipeline must still be refused — it is the "
              "thing feeding the terminal";
  } catch (const std::exception& ex) {
    EXPECT_NE(std::string(ex.what()).find("would receive a Record"),
              std::string::npos) << ex.what();
  }
}

} // namespace
