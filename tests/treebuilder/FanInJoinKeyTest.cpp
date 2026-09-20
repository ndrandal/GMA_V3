// ENC-1292 — the DECLARED join key: `by` is `"streamKey"`, `"none"`, or a
// BUILD ERROR.
//
// SPEC specs/2026-09-20-gma-join-correctness D1, D6, section 1.1 defect 3, and
// section 5 Q3 (RULED) / Q6.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE DEFECT THIS FILE CLOSES
//
// Both fan-in nodes keyed their pending state on `sv.symbol`, so NO JOIN ACROSS
// TWO STREAMKEYS WAS POSSIBLE ANYWHERE IN THE ENGINE — not a bug in one node,
// the expressiveness ceiling of the system. 23 of the 52 `Aggregate` requests
// in the checked-in corpus ask for exactly that join, and this file measures
// all 23 rather than asserting the number (block 3).
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY HALF OF THIS FILE IS ABOUT REJECTION
//
// `JsonValidator::validateTree` is an explicit open-vocabulary walk — it bounds
// string length, array size and depth and NEVER looks at a key — and the
// builders read named members through `strOr`/`sizeOr` WITH DEFAULTS. So a
// `by` the builder does not recognise would pass validation, silently take the
// `"streamKey"` default, and return a PLAUSIBLE WRONG NUMBER with no
// diagnostic in either repo. That is the failure shape D7 exists to convert
// into a loud one, and it is why Q3 ruled `by` a CLOSED vocabulary rather than
// merely reserving a name for later. Block 2 is that ruling.
//
// What each block gates:
//   1. the two accepted modes, end to end through the real builder, including
//      output identity (Q6)
//   2. the closed vocabulary: `"origin"` reserved by name, everything else —
//      mis-casing included — refused rather than defaulted
//   3. the corpus: all 23 cross-streamKey `Aggregate` requests, before and
//      after, measured rather than asserted

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/JoinBy.hpp"
#include "gma/rt/ThreadPool.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace gma;

// Records BOTH the value and the symbol. The symbol is the point of half this
// file: `ClientSession` serialises `sv.symbol` as the frame's `streamKey`, so
// a join's output identity IS whatever the last node put there (SPEC Q6).
class Sink final : public INode {
public:
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    ++arrivals_;
    symbols_[sv.symbol]++;
    if (const double* d = std::get_if<double>(&sv.value)) vals_.push_back(*d);
    else                                                  ++nonNumeric_;
  }
  void shutdown() noexcept override {}

  std::size_t arrivals() const {
    std::lock_guard<std::mutex> lk(mx_); return arrivals_;
  }
  std::vector<double> values() const {
    std::lock_guard<std::mutex> lk(mx_); return vals_;
  }
  std::map<std::string, std::size_t> symbols() const {
    std::lock_guard<std::mutex> lk(mx_); return symbols_;
  }
  std::size_t nonNumeric() const {
    std::lock_guard<std::mutex> lk(mx_); return nonNumeric_;
  }
  std::string renderSymbols() const {
    std::ostringstream os;
    for (const auto& [s, n] : symbols()) os << " " << s << "=" << n;
    return os.str();
  }

private:
  mutable std::mutex                  mx_;
  std::vector<double>                 vals_;
  std::map<std::string, std::size_t>  symbols_;
  std::size_t                         arrivals_{0};
  std::size_t                         nonNumeric_{0};
};

rapidjson::Document parse(const char* json) {
  rapidjson::Document d;
  d.Parse(json);
  EXPECT_FALSE(d.HasParseError()) << "test fixture JSON is malformed";
  return d;
}

// Same search paths as CorpusTest / FanInArityTest. A missing corpus must be
// LOUD, never a skip (ENC-807 L17).
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

class FanInJoinKey : public ::testing::Test {
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

  void tickFields(const std::string& symbol,
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

  // Build, run `drive`, drain, tear the chain down. Returns the sink.
  std::shared_ptr<Sink> run(const rapidjson::Value& request,
                            const std::function<void()>& drive) {
    auto sink  = std::make_shared<Sink>();
    auto chain = tree::buildForRequest(request, deps_, sink);
    drive();
    pool_->drain();
    for (auto& n : chain.keepAlive) if (n) n->shutdown();
    if (chain.head) chain.head->shutdown();
    return sink;
  }

  // Returns the builder's message, or "" if the build was ACCEPTED.
  std::string refusalFor(const rapidjson::Value& request) {
    auto sink = std::make_shared<Sink>();
    try {
      auto chain = tree::buildForRequest(request, deps_, sink);
      for (auto& n : chain.keepAlive) if (n) n->shutdown();
      if (chain.head) chain.head->shutdown();
      return "";
    } catch (const std::exception& ex) {
      return ex.what();
    }
  }

  AtomicStore                     store_;
  std::shared_ptr<rt::ThreadPool> pool_;
  std::shared_ptr<rt::ThreadPool> prevPool_;
  std::unique_ptr<Dispatcher>     dispatcher_;
  tree::Deps                      deps_;
};

// Corpus 87's shape, spelled out here so block 1 does not depend on the corpus
// file: "price difference between AAPL and MSFT", the `node` alone (the
// pipeline is stripped so the terminal sees the JOIN's own members rather than
// `Worker{fn:"diff"}` output — the same observation point CorpusTest uses).
const char* kCrossRequest(const char* byClause) {
  static std::string s;
  s = std::string(R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,)") + byClause +
      R"("inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})";
  return s.c_str();
}

} // namespace

// ═══ 1. The two accepted modes, end to end ═══════════════════════════════════

// D6's no-migration guarantee, gated. A stored forum graph carries no `by` —
// `by` did not exist when it was stored — so the default must be bit-for-bit
// what shipped before. For a cross-streamKey request that means EMITTING
// NOTHING: AAPL fills port 0 of one buffer, MSFT port 1 of another, neither
// completes.
TEST_F(FanInJoinKey, NoByMeansStreamKeyAndACrossSymbolJoinStillEmitsNothing) {
  rapidjson::Document implicitDefault = parse(kCrossRequest(""));
  rapidjson::Document explicitDefault = parse(kCrossRequest("\"by\":\"streamKey\","));

  for (auto* req : {&implicitDefault, &explicitDefault}) {
    auto sink = run(*req, [&] {
      for (int n = 0; n < 6; ++n) {
        tick("AAPL", {{"lastPrice", 1000.0 + n}});
        tick("MSFT", {{"lastPrice", 5000.0 + n}});
      }
    });
    EXPECT_EQ(sink->arrivals(), 0u)
        << "12 ticks produced " << sink->arrivals() << " arrival(s) under the "
           "DEFAULT join key. The default must remain `by:\"streamKey\"` and "
           "must not join across symbols — every stored forum graph would "
           "otherwise change meaning with no migration available (SPEC D6). "
           "Symbols seen:" << sink->renderSymbols();
  }
}

// THE gate for SPEC section 1.1 defect 3, through the real builder.
TEST_F(FanInJoinKey, ByNoneJoinsAcrossTwoStreamKeysThroughTheRealBuilder) {
  rapidjson::Document req = parse(kCrossRequest("\"by\":\"none\","));

  auto sink = run(req, [&] {
    for (int n = 0; n < 6; ++n) {
      tick("AAPL", {{"lastPrice", 1000.0 + n}});
      tick("MSFT", {{"lastPrice", 5000.0 + n}});
    }
  });

  // ── ANTI-VACUITY. Everything below is a statement about emitted tuples;
  // over an empty list every one of them holds. The join must emit FIRST.
  ASSERT_EQ(sink->arrivals(), 12u)
      << "6 ticks on each side must complete 6 two-member tuples = 12 "
         "arrivals; got " << sink->arrivals() << ". Symbols seen:"
      << sink->renderSymbols();

  // Each tuple is one AAPL value and one MSFT value, in DECLARED PORT ORDER.
  const std::vector<double> v = sink->values();
  ASSERT_EQ(v.size(), 12u);
  for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
    EXPECT_DOUBLE_EQ(v[i],     1000.0 + double(i / 2)) << "tuple " << (i / 2)
        << " member 0 must be AAPL's own value (port 0 is the AAPL Listener)";
    EXPECT_DOUBLE_EQ(v[i + 1], 5000.0 + double(i / 2)) << "tuple " << (i / 2)
        << " member 1 must be MSFT's own value — pairing one side with itself "
           "is the defect, not the fix";
  }

  // SPEC section 5 Q6 — OUTPUT IDENTITY. Every arrival carries the REQUEST'S
  // top-level streamKey, not the symbol of whichever side released the tuple.
  const auto syms = sink->symbols();
  ASSERT_EQ(syms.size(), 1u)
      << "the joined stream must have ONE identity; got" << sink->renderSymbols()
      << ". `ClientSession` serialises `sv.symbol` as the frame's streamKey, "
         "so two identities here is two streams on one request key.";
  EXPECT_EQ(syms.begin()->first, "AAPL")
      << "a by:\"none\" join emits under the request's own top-level "
         "'streamKey'. `sv.symbol` at the point of emission is MSFT here (the "
         "side that released the tuple) and would be AAPL with the ticks "
         "interleaved the other way — an identity that depends on arrival "
         "order is a race, not an identity (SPEC section 5 Q6).";
  EXPECT_EQ(sink->nonNumeric(), 0u);
}

// The head Listener's own streamKey does not have to be one of the join's
// inputs, and the output identity follows the REQUEST, not the inputs. This
// separates "emits under the request's streamKey" from "emits under input 0's
// streamKey", which the test above cannot distinguish — corpus 87's top-level
// streamKey happens to equal its first input's.
TEST_F(FanInJoinKey, ByNoneOutputIdentityIsTheRequestKeyNotInputZeros) {
  rapidjson::Document req = parse(R"({
    "key":1,"streamKey":"PAIR","field":"lastPrice",
    "node":{"type":"Aggregate","arity":2,"by":"none","inputs":[
      {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
      {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})");

  auto sink = run(req, [&] {
    for (int n = 0; n < 3; ++n) {
      tick("AAPL", {{"lastPrice", 1000.0 + n}});
      tick("MSFT", {{"lastPrice", 5000.0 + n}});
    }
  });

  ASSERT_EQ(sink->arrivals(), 6u) << "symbols:" << sink->renderSymbols();
  const auto syms = sink->symbols();
  ASSERT_EQ(syms.size(), 1u) << sink->renderSymbols();
  EXPECT_EQ(syms.begin()->first, "PAIR")
      << "got" << sink->renderSymbols() << ". The joined stream's identity is "
         "the REQUEST's 'streamKey' — substituting input 0's symbol would be "
         "indistinguishable on corpus 87 and wrong here.";
}

// `Pack` keys on `sv.symbol` the same way and gets the same declared key.
// Measured before ENC-1292: `Pack{a:AAPL, b:MSFT}` with 6 ticks each side
// emitted 0 records, forever, silently.
TEST_F(FanInJoinKey, ByNoneAlsoWorksForPack) {
  // D7 forbids a Record reaching the terminal, so the canonical shape applies:
  // Pack -> Field -> scalar.
  rapidjson::Document req = parse(R"({
    "key":1,"streamKey":"PAIR","field":"lastPrice",
    "node":{"type":"Pack","by":"none","fields":{
      "a":{"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
      "b":{"type":"Listener","streamKey":"MSFT","field":"lastPrice"}}},
    "pipeline":[{"type":"Field","name":"b"}]})");

  auto sink = run(req, [&] {
    for (int n = 0; n < 3; ++n) {
      tick("AAPL", {{"lastPrice", 1000.0 + n}});
      tick("MSFT", {{"lastPrice", 5000.0 + n}});
    }
  });

  ASSERT_GT(sink->arrivals(), 0u)
      << "Pack{a:AAPL, b:MSFT} assembled NO record from 6 ticks. Under "
         "by:\"none\" the two symbols share one state and complete each other.";
  const auto syms = sink->symbols();
  ASSERT_EQ(syms.size(), 1u) << sink->renderSymbols();
  EXPECT_EQ(syms.begin()->first, "PAIR") << sink->renderSymbols();
  for (double v : sink->values())
    EXPECT_GE(v, 5000.0) << "field 'b' is MSFT's value; got " << v;
}

TEST_F(FanInJoinKey, PackDefaultStillNeverAssemblesAcrossSymbols) {
  rapidjson::Document req = parse(R"({
    "key":1,"streamKey":"PAIR","field":"lastPrice",
    "node":{"type":"Pack","fields":{
      "a":{"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
      "b":{"type":"Listener","streamKey":"MSFT","field":"lastPrice"}}},
    "pipeline":[{"type":"Field","name":"b"}]})");

  auto sink = run(req, [&] {
    for (int n = 0; n < 3; ++n) {
      tick("AAPL", {{"lastPrice", 1000.0 + n}});
      tick("MSFT", {{"lastPrice", 5000.0 + n}});
    }
  });
  EXPECT_EQ(sink->arrivals(), 0u)
      << "the default must not start joining across symbols (SPEC D6)"
      << sink->renderSymbols();
}

// ENC-1290's addendum to this ticket: corpus 115 is the ONE cross-streamKey
// entry whose inputs are PULL nodes, so it exercises `by:"none"` and D5's
// clock rule together and is the only corpus entry that does. Measured before
// ENC-1292 (q1-probe S7): four arrivals under TWO symbols, `max(70,70)` and
// `max(30,30)` forever — the clock rule works and the two streams never meet.
TEST_F(FanInJoinKey, ByNoneJoinsPullOnlyInputsClockedByTheOuterListener) {
  store_.set("AAPL", "rsi_14", 70.0);
  store_.set("MSFT", "rsi_14", 30.0);

  rapidjson::Document req = parse(R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Aggregate","arity":2,"by":"none","inputs":[
      {"type":"AtomicAccessor","streamKey":"AAPL","field":"rsi_14"},
      {"type":"AtomicAccessor","streamKey":"MSFT","field":"rsi_14"}]}})");

  auto sink = run(req, [&] {
    for (int n = 0; n < 3; ++n) tick("AAPL", {{"lastPrice", 100.0 + n}});
  });

  ASSERT_EQ(sink->arrivals(), 6u)
      << "3 clock ticks must complete 3 two-member tuples; got "
      << sink->arrivals() << ". Symbols:" << sink->renderSymbols();
  const std::vector<double> v = sink->values();
  for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
    EXPECT_DOUBLE_EQ(v[i],     70.0) << "port 0 is AAPL's rsi_14";
    EXPECT_DOUBLE_EQ(v[i + 1], 30.0) << "port 1 is MSFT's rsi_14 — before "
        "ENC-1292 each side completed with ITSELF, so this position held 70";
  }
  const auto syms = sink->symbols();
  ASSERT_EQ(syms.size(), 1u) << sink->renderSymbols();
  EXPECT_EQ(syms.begin()->first, "AAPL") << sink->renderSymbols();
}

// ═══ 2. The closed vocabulary (SPEC section 5 Q3, ruled) ═════════════════════

// Q3's core: `"origin"` is RESERVED, and a client that tries it must learn that
// rather than receive a plausible wrong number computed under the default.
TEST_F(FanInJoinKey, ByOriginIsRefusedAsReservedAndUnimplemented) {
  rapidjson::Document req = parse(kCrossRequest("\"by\":\"origin\","));
  const std::string msg = refusalFor(req);

  ASSERT_FALSE(msg.empty())
      << "by:\"origin\" was ACCEPTED. It would have taken the `\"streamKey\"` "
         "default silently — `JsonValidator` never looks at a key — and "
         "returned a per-symbol answer to a request asking for a "
         "same-upstream-event join.";
  EXPECT_NE(msg.find("origin"), std::string::npos)
      << "the message must name the value. Got: " << msg;
  EXPECT_NE(msg.find("RESERVED"), std::string::npos)
      << "the message must say the name is RESERVED, so a future "
         "same-upstream-event join cannot ship under a different spelling "
         "(SPEC section 5 Q3). Got: " << msg;
  EXPECT_NE(msg.find("NOT IMPLEMENTED"), std::string::npos)
      << "\"reserved\" alone reads as \"you may not use this\"; the author "
         "also needs to know the semantics do not exist yet. Got: " << msg;
  EXPECT_NE(msg.find("Aggregate"), std::string::npos)
      << "a request may carry several fan-ins; the message must name which "
         "one. Got: " << msg;
}

// The half of Q3 that carries most of the value: the TYPO. Reserving a name
// without this leaves the actual hole open.
TEST_F(FanInJoinKey, MisCasedAndUnknownByValuesAreRefusedNotDefaulted) {
  struct Case { const char* by; const char* why; };
  const Case cases[] = {
    {"streamkey",  "mis-cased — the closest possible miss, and the one a "
                   "hand-written request is most likely to make"},
    {"StreamKey",  "mis-cased the other way"},
    {"NONE",       "mis-cased 'none'"},
    {"symbol",     "a plausible synonym that is not the spelling"},
    {"typo",       "an outright unknown value"},
    {"",           "the empty string — present, a string, and meaningless"},
  };

  for (const auto& c : cases) {
    const std::string clause =
        std::string("\"by\":\"") + c.by + "\",";
    rapidjson::Document req = parse(kCrossRequest(clause.c_str()));
    const std::string msg = refusalFor(req);

    ASSERT_FALSE(msg.empty())
        << "by:\"" << c.by << "\" was ACCEPTED (" << c.why << "). It would "
           "have silently taken the `\"streamKey\"` default and returned a "
           "plausible wrong number with no diagnostic in either repo — the "
           "precise failure SPEC section 5 Q3 ruled against.";
    EXPECT_NE(msg.find("CLOSED vocabulary"), std::string::npos)
        << "the message must say `by` is a CLOSED vocabulary, so the author "
           "knows the value was refused rather than mis-parsed. Got: " << msg;
    EXPECT_NE(msg.find("case-sensitive"), std::string::npos)
        << "mis-casing is the likeliest miss and the message must call it "
           "out by name. Got: " << msg;
    // ...and it must NOT be mistaken for the reserved-name path.
    EXPECT_EQ(msg.find("RESERVED"), std::string::npos)
        << "by:\"" << c.by << "\" is unknown, not reserved; conflating the two "
           "tells the author to wait for a feature that will never name their "
           "spelling. Got: " << msg;
  }
}

TEST_F(FanInJoinKey, NonStringByIsRefused) {
  // `strOr`-shaped code would silently default on every one of these.
  const char* bys[] = {"2", "true", "null", "[\"none\"]", "{\"k\":\"none\"}"};
  for (const char* b : bys) {
    const std::string clause = std::string("\"by\":") + b + ",";
    rapidjson::Document req = parse(kCrossRequest(clause.c_str()));
    const std::string msg = refusalFor(req);
    ASSERT_FALSE(msg.empty())
        << "by:" << b << " was ACCEPTED and would have taken the default";
    EXPECT_NE(msg.find("'by' must be a string"), std::string::npos)
        << "for by:" << b << ", got: " << msg;
  }
}

TEST_F(FanInJoinKey, PackRejectsTheSameVocabularyAndNamesItself) {
  rapidjson::Document req = parse(R"({
    "key":1,"streamKey":"PAIR","field":"lastPrice",
    "node":{"type":"Pack","by":"origin","fields":{
      "a":{"type":"Listener","streamKey":"AAPL","field":"lastPrice"}}},
    "pipeline":[{"type":"Field","name":"a"}]})");
  const std::string msg = refusalFor(req);
  ASSERT_FALSE(msg.empty()) << "Pack accepted by:\"origin\"";
  EXPECT_NE(msg.find("Pack"), std::string::npos) << msg;
  EXPECT_NE(msg.find("RESERVED"), std::string::npos) << msg;
}

// SPEC section 5 Q6, the build-time half. A join that ignores the symbol has no
// identity unless it inherits one. `buildForRequest` refuses an empty top-level
// `streamKey`, so this is reachable through `buildTree`/`buildNode`, where
// there is no default in scope at all.
TEST_F(FanInJoinKey, ByNoneWithNoRequestStreamKeyInScopeIsRefused) {
  rapidjson::Document node = parse(R"({"type":"Aggregate","arity":2,"by":"none",
      "inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]})");

  std::string msg;
  try {
    auto n = tree::buildTree(node, deps_);
    if (n) n->shutdown();
    FAIL() << "by:\"none\" with no streamKey in scope was ACCEPTED. The join "
              "would emit under the EMPTY symbol, which `ClientSession` would "
              "serialise as `\"streamKey\":\"\"` — a silent wrong answer on "
              "the wire.";
  } catch (const std::exception& ex) {
    msg = ex.what();
  }
  // ASSERT ON THE BUILDER'S OWN PHRASE, NOT ON THE SHARED ONE (ENC-1292,
  // mutation M7). This invariant is delivered by TWO independent mechanisms —
  // the builder's check here and `Aggregate`'s constructor invariant — and
  // both messages contain "top-level 'streamKey'". Asserting that substring
  // therefore passed with the BUILDER's check deleted, because the
  // constructor's throw satisfied it: a test that claims to gate the builder
  // while any of two mechanisms can satisfy it is not gating the builder.
  // "Build this node through buildForRequest" occurs only in the builder's
  // message. The constructor's own half is gated separately, by
  // `AggregateTest.ByNoneWithNoOutputStreamKeyIsRefused` (red under M8).
  EXPECT_NE(msg.find("Build this node through buildForRequest"),
            std::string::npos)
      << "the BUILDER must refuse this, with its own diagnostic naming the "
         "JSON-level remedy. Got: " << msg;
  EXPECT_NE(msg.find("top-level 'streamKey'"), std::string::npos)
      << "the message must name what is missing. Got: " << msg;

  // The default mode needs no such identity and must still build here.
  rapidjson::Document dflt = parse(R"({"type":"Aggregate","arity":2,"inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]})");
  auto ok = tree::buildTree(dflt, deps_);
  ASSERT_NE(ok, nullptr);
  ok->shutdown();
}

// A refused `by` must not strand a subscribed Listener. The check runs before
// the fan-in's own inputs are built, but a NESTED fan-in's bad `by` is reached
// after the OUTER fan-in has already built and subscribed input 0 — which is
// the hole `SubBuildUnwind` exists for (SPEC section 1.5).
TEST_F(FanInJoinKey, RefusedByLeavesNothingSubscribed) {
  const char* kReqs[] = {
    // top-level bad `by`: nothing should be built at all
    R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
        "node":{"type":"Aggregate","arity":2,"by":"origin","inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"ask"},
          {"type":"Listener","streamKey":"AAPL","field":"bid"}]}})",
    // nested bad `by`: input 0's Listener is real and already subscribed
    R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
        "node":{"type":"Aggregate","arity":2,"inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"ask"},
          {"type":"Aggregate","arity":1,"by":"typo","inputs":[
            {"type":"Listener","streamKey":"AAPL","field":"bid"}]}]}})",
  };

  const std::size_t before = dispatcher_->subscriptionCount();
  constexpr int kRejects = 25;

  for (const char* json : kReqs) {
    rapidjson::Document d = parse(json);
    int rejected = 0;
    for (int i = 0; i < kRejects; ++i)
      if (!refusalFor(d).empty()) ++rejected;
    ASSERT_EQ(rejected, kRejects)
        << "this request must be REFUSED for the test to mean anything";
  }

  EXPECT_EQ(dispatcher_->subscriptionCount(), before)
      << (dispatcher_->subscriptionCount() - before) << " subscription(s) "
         "survived " << (2 * kRejects) << " REFUSED builds. A stranded "
         "Listener is walked on every onTick forever with nothing left holding "
         "a handle that could stop it, and it is reachable from client JSON in "
         "a loop (SPEC section 1.5).";
}

// ═══ 3. The corpus — all 23, measured rather than asserted ═══════════════════
//
// SPEC section 1.1 defect 3: "23 of the 52 Aggregate requests ask for a
// cross-symbol join that cannot be performed at all". This test re-derives the
// 23 from the corpus, drives every one of them BOTH ways, and pins both halves:
// zero emissions under the unchanged default (D6) and a complete tuple under
// `by:"none"`. Re-deriving rather than hardcoding the ids means a corpus that
// gains or loses a cross-streamKey entry goes red here rather than silently
// re-measuring a stale number.
//
// The corpus file is NOT modified: `by` is injected into the COPY, which is
// what an authored cross-symbol request would carry once forum can emit it.
namespace {

// Collect every (streamKey, field) pair declared inside a spec subtree.
void collectPairs(const rapidjson::Value& v,
                  std::set<std::pair<std::string, std::string>>& out) {
  if (v.IsArray()) {
    for (const auto& e : v.GetArray()) collectPairs(e, out);
    return;
  }
  if (!v.IsObject()) return;
  if (v.HasMember("streamKey") && v["streamKey"].IsString() &&
      v.HasMember("field") && v["field"].IsString())
    out.emplace(v["streamKey"].GetString(), v["field"].GetString());
  for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
    collectPairs(m->value, out);
}

// Is this spec an `Aggregate`/`Pack` whose declared inputs span more than one
// streamKey? Structural, over the request JSON — the same kind of test
// `declaredInputIsSelfClocked` makes.
bool isCrossStreamKeyFanIn(const rapidjson::Value& v) {
  if (!v.IsObject() || !v.HasMember("type") || !v["type"].IsString()) return false;
  const std::string t = v["type"].GetString();
  if (t != "Aggregate" && t != "Pack") return false;
  std::set<std::pair<std::string, std::string>> pairs;
  collectPairs(v, pairs);
  std::set<std::string> keys;
  for (const auto& p : pairs) keys.insert(p.first);
  return keys.size() > 1;
}

// Set `by` on every cross-streamKey fan-in in a copied request.
std::size_t injectBy(rapidjson::Value& v, const char* by,
                     rapidjson::Document::AllocatorType& al) {
  std::size_t n = 0;
  if (v.IsArray()) {
    for (auto& e : v.GetArray()) n += injectBy(e, by, al);
    return n;
  }
  if (!v.IsObject()) return 0;
  if (isCrossStreamKeyFanIn(v)) {
    v.RemoveMember("by");
    v.AddMember("by", rapidjson::Value(by, al).Move(), al);
    ++n;
  }
  for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
    n += injectBy(m->value, by, al);
  return n;
}

bool requestHasCrossStreamKeyFanIn(const rapidjson::Value& v) {
  if (v.IsArray()) {
    for (const auto& e : v.GetArray())
      if (requestHasCrossStreamKeyFanIn(e)) return true;
    return false;
  }
  if (!v.IsObject()) return false;
  if (isCrossStreamKeyFanIn(v)) return true;
  for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
    if (requestHasCrossStreamKeyFanIn(m->value)) return true;
  return false;
}

} // namespace

TEST_F(FanInJoinKey, EveryCrossStreamKeyCorpusRequestJoinsUnderByNone) {
  rapidjson::Document& corpus = corpusDoc();
  ASSERT_FALSE(corpus.IsNull()) << "corpus_requests.json not found — run the "
      "suite from the directory CMake copies it into (add_test sets "
      "WORKING_DIRECTORY; a hand-run from elsewhere is the failure)";
  ASSERT_TRUE(corpus.IsArray());

  struct Row { int id; std::size_t injected; std::size_t byDefault; std::size_t byNone; };
  std::vector<Row> rows;

  for (const auto& entry : corpus.GetArray()) {
    if (!entry.IsObject() || !entry.HasMember("request")) continue;
    const rapidjson::Value& request = entry["request"];
    if (!request.IsObject() || !requestHasCrossStreamKeyFanIn(request)) continue;
    const int id = entry.HasMember("corpus_id") && entry["corpus_id"].IsInt()
                     ? entry["corpus_id"].GetInt() : -1;

    // Drive the `node` alone: the observation point is the JOIN's own members,
    // not what a `Worker` downstream makes of them (the same reason CorpusTest
    // strips the pipeline). The corpus file itself is untouched.
    rapidjson::Document base;
    base.CopyFrom(request, base.GetAllocator());
    base.RemoveMember("pipeline");
    base.RemoveMember("stages");

    std::set<std::pair<std::string, std::string>> pairs;
    collectPairs(base, pairs);
    // ...plus the head Listener's own (streamKey, field), which is the clock.
    if (base.HasMember("streamKey") && base.HasMember("field"))
      pairs.emplace(base["streamKey"].GetString(), base["field"].GetString());

    std::map<std::string, std::vector<std::pair<std::string, double>>> byKey;
    double seed = 100.0;
    for (const auto& [sk, f] : pairs) {
      seed += 10.0;
      byKey[sk].emplace_back(f, seed);
      store_.set(sk, f, seed);            // for AtomicAccessor (pull) inputs
    }

    auto drive = [&] {
      for (int n = 0; n < 3; ++n)
        for (const auto& [sk, fields] : byKey) tickFields(sk, fields);
    };

    Row row{id, 0, 0, 0};

    rapidjson::Document dflt;
    dflt.CopyFrom(base, dflt.GetAllocator());
    row.byDefault = run(dflt, drive)->arrivals();

    rapidjson::Document none;
    none.CopyFrom(base, none.GetAllocator());
    row.injected = injectBy(none, "none", none.GetAllocator());
    row.byNone   = run(none, drive)->arrivals();

    rows.push_back(row);
  }

  // The denominator, re-derived. SPEC section 1.1 defect 3 measured 23.
  ASSERT_EQ(rows.size(), 23u)
      << "expected 23 cross-streamKey fan-in requests in the corpus (SPEC "
         "section 1.1 defect 3); found " << rows.size() << ". If the corpus "
         "changed, re-measure the SPEC's number rather than editing this one.";

  std::size_t stillSilent = 0, nowJoins = 0, defaultMoved = 0;
  std::ostringstream detail;
  for (const auto& r : rows) {
    if (r.byDefault != 0) { ++defaultMoved;
      detail << "\n      corpus " << r.id << ": DEFAULT emitted "
             << r.byDefault << " (must be 0)"; }
    if (r.byNone == 0)    { ++stillSilent;
      detail << "\n      corpus " << r.id << ": by:\"none\" emitted NOTHING"; }
    else ++nowJoins;
    EXPECT_GT(r.injected, 0u) << "corpus " << r.id
        << ": no cross-streamKey fan-in was found to inject `by` into, yet the "
           "request was selected as one — the two walks disagree";
  }

  EXPECT_EQ(defaultMoved, 0u)
      << defaultMoved << " of 23 cross-streamKey requests EMIT under the "
         "unchanged default. The default must stay `by:\"streamKey\"`, under "
         "which two streamKeys never complete a tuple — SPEC D6's "
         "no-migration guarantee for stored forum graphs is exactly this."
      << detail.str();

  EXPECT_EQ(nowJoins, 23u)
      << nowJoins << " of 23 cross-streamKey corpus requests join under "
         "by:\"none\"; " << stillSilent << " still emit nothing. Before "
         "ENC-1292 the number that could join was 0 — the correlation key was "
         "`sv.symbol` and nothing else (SPEC section 1.1 defect 3)."
      << detail.str();
}
