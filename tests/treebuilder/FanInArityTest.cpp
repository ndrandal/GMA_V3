// ENC-1291 — `arity` must equal `inputs.size()`, and the builder must say so.
//
// SPEC specs/2026-09-20-gma-join-correctness D2, section 1.1 defect 2.
//
// ─────────────────────────────────────────────────────────────────────────────
// THE DEFECT THIS FILE CLOSES
//
// `arity` was read by `sizeOr` and NEVER compared to `inputs.size()`, so
//
//     {"type":"Aggregate","arity":2,"inputs":[a,b,c,d,e]}
//
// built happily. It was harmless-looking only because the join counted VALUES:
// with no input identity, "2" and "five inputs" were never in contact. Now that
// an input's identity IS its port index the two numbers are the same number
// said twice, and there is no defensible reading of "2 of these 5 inputs" —
// so a disagreement is refused, naming both counts.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT IT COSTS, MEASURED RATHER THAN ASSERTED
//
// `EveryCorpusAggregateDeclaresArityEqualToItsInputCount` walks all 272
// checked-in corpus requests and reports the refusal count. It is **0**, and
// the test pins the 52-entry denominator as well, so a future corpus that
// gained a mismatching entry — or lost the `Aggregate`s this measurement is
// about — goes red rather than silently re-measuring zero out of nothing.

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

#include <fstream>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace gma;

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

// Same search paths as CorpusTest / ComposedChainTest. A missing corpus must be
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

// Walk a request's node/pipeline/stages subtrees, visiting every object that
// declares a "type".
void walkSpecs(const rapidjson::Value& v,
               const std::function<void(const rapidjson::Value&)>& fn) {
  if (v.IsArray()) {
    for (const auto& e : v.GetArray()) walkSpecs(e, fn);
    return;
  }
  if (!v.IsObject()) return;
  if (v.HasMember("type") && v["type"].IsString()) fn(v);
  for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) walkSpecs(m->value, fn);
}

class FanInArity : public ::testing::Test {
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

  AtomicStore                     store_;
  std::shared_ptr<rt::ThreadPool> pool_;
  std::shared_ptr<rt::ThreadPool> prevPool_;
  std::unique_ptr<Dispatcher>     dispatcher_;
  tree::Deps                      deps_;
};

} // namespace

// ═══ 1. The refusal, both directions ═════════════════════════════════════════

TEST_F(FanInArity, ArityBelowInputCountIsRefusedNamingBothCounts) {
  const char* kReq = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Aggregate","arity":2,"inputs":[
      {"type":"Listener","streamKey":"AAPL","field":"a"},
      {"type":"Listener","streamKey":"AAPL","field":"b"},
      {"type":"Listener","streamKey":"AAPL","field":"c"},
      {"type":"Listener","streamKey":"AAPL","field":"d"},
      {"type":"Listener","streamKey":"AAPL","field":"e"}]}
  })";
  rapidjson::Document d;
  d.Parse(kReq);
  ASSERT_FALSE(d.HasParseError());

  auto sink = std::make_shared<Sink>();
  std::string msg;
  try {
    tree::buildForRequest(d, deps_, sink);
    FAIL() << "{\"arity\":2,\"inputs\":[a,b,c,d,e]} was ACCEPTED. Before "
              "ENC-1291 it built happily and the join then counted values "
              "instead of inputs (SPEC D2).";
  } catch (const std::exception& ex) {
    msg = ex.what();
  }

  EXPECT_NE(msg.find("Aggregate"), std::string::npos) << msg;

  // ASSERT ON THE PHRASE, NOT ON THE DIGIT. `msg.find("2")` — what this test
  // did first — is satisfied by the `2026-09-20` in the message's own SPEC
  // path, so replacing `std::to_string(arity)` with a literal left this green
  // with the declared arity gone entirely (measured, ENC-1291 adversarial
  // pass). A substring that an unrelated part of the same string can satisfy
  // is not an assertion about the number.
  EXPECT_NE(msg.find("'arity' is 2"), std::string::npos)
      << "the message must name the DECLARED arity. Got: " << msg;
  EXPECT_NE(msg.find("'inputs' declares 5 input(s)"), std::string::npos)
      << "the message must name the ACTUAL input count, otherwise the author "
         "cannot see which of the two numbers they meant. Got: " << msg;
}

TEST_F(FanInArity, ArityAboveInputCountIsRefusedNamingBothCounts) {
  const char* kReq = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Aggregate","arity":5,"inputs":[
      {"type":"Listener","streamKey":"AAPL","field":"ask"},
      {"type":"Listener","streamKey":"AAPL","field":"bid"}]}
  })";
  rapidjson::Document d;
  d.Parse(kReq);
  ASSERT_FALSE(d.HasParseError());

  auto sink = std::make_shared<Sink>();
  std::string msg;
  try {
    tree::buildForRequest(d, deps_, sink);
    FAIL() << "{\"arity\":5,\"inputs\":[ask,bid]} was ACCEPTED. Before ENC-1291 "
              "this built a join that waited for FIVE values and completed one "
              "\"tuple\" out of five ticks of whichever input fired.";
  } catch (const std::exception& ex) {
    msg = ex.what();
  }
  // The mirror of the direction above: here the declared arity is 5 and the
  // actual count is 2, so a literal substituted for either number reddens
  // exactly one of the two tests. Neither digit can be satisfied by the SPEC
  // path in the message.
  EXPECT_NE(msg.find("'arity' is 5"), std::string::npos) << msg;
  EXPECT_NE(msg.find("'inputs' declares 2 input(s)"), std::string::npos) << msg;
}

// The mirror of the refusals: the matching form must still build AND emit.
// A check written one condition too broad is silent in the other direction,
// which is the failure mode SPEC Q7 names for exactly this kind of guard.
TEST_F(FanInArity, MatchingArityBuildsAndJoinsByInput) {
  const char* kReq = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Aggregate","arity":2,"inputs":[
      {"type":"Listener","streamKey":"AAPL","field":"ask"},
      {"type":"Listener","streamKey":"AAPL","field":"bid"}]}
  })";
  rapidjson::Document d;
  d.Parse(kReq);
  ASSERT_FALSE(d.HasParseError());

  auto sink  = std::make_shared<Sink>();
  auto chain = tree::buildForRequest(d, deps_, sink);
  ASSERT_NE(chain.head, nullptr);

  // Only `ask`. `bid` never fires: no tuple can complete.
  for (int n = 0; n < 8; ++n) tick("AAPL", {{"ask", 1000.02 + n}});
  pool_->drain();
  EXPECT_EQ(sink->values().size(), 0u)
      << "8 `ask` values with no `bid` produced " << sink->values().size()
      << " arrival(s): " << render(sink->values())
      << "\n    A two-input join has no complete tuple to emit (SPEC D2).";

  // Now both sides, one tick.
  tick("AAPL", {{"ask", 2000.02}, {"bid", 2000.00}});
  pool_->drain();
  ASSERT_EQ(sink->values().size(), 2u) << render(sink->values());
  EXPECT_DOUBLE_EQ(sink->values()[0], 2000.02) << "inputs[0] (`ask`) first";
  EXPECT_DOUBLE_EQ(sink->values()[1], 2000.00) << "inputs[1] (`bid`) second";
}

// ═══ 2. The refusal happens BEFORE anything subscribes ═══════════════════════
//
// The `Listener` builder registers with the Dispatcher as it is constructed and
// the only thing that unregisters is `Listener::shutdown()` (SPEC section 1.5).
// `buildForRequest`'s RAII unwind guard covers a late throw, but the arity check
// is deliberately placed before a single node is constructed, and this asserts
// that placement rather than trusting it.
//
// THIS TEST'S FIRST DRAFT WAS FAKE, and the way it failed is the reason
// `Dispatcher::subscriptionCount()` now exists.
//
// It asserted that no VALUE reached the terminal of a rejected build. That is
// 0 whether the check runs before or after the input loop, because a `Listener`
// holds its downstream through a `weak_ptr`: once the rejected `Aggregate` and
// its ports are destroyed, a stranded Listener delivers to nobody. It is still
// SUBSCRIBED, still walked on every `Dispatcher::onTick`, and still
// unreachable by anything that could stop it — which IS the defect (SPEC
// section 1.5: 1000 rejected builds took `onTick` x 50 from 0.02 ms to 932 ms,
// with no bound). Moving the `arity != inputCount` throw to after the whole
// input loop left the old assertion green (measured, ENC-1291 adversarial
// pass).
//
// So it now counts SUBSCRIPTIONS, which is the thing section 1.5 is about, and
// keeps the value check as a second, weaker statement.
TEST_F(FanInArity, ArityMismatchIsRefusedBeforeAnythingSubscribes) {
  const char* kReq = R"({
    "key":1,"streamKey":"AAPL","field":"lastPrice",
    "node":{"type":"Aggregate","arity":3,"inputs":[
      {"type":"Listener","streamKey":"AAPL","field":"ask"},
      {"type":"Listener","streamKey":"AAPL","field":"bid"}]}
  })";
  rapidjson::Document d;
  d.Parse(kReq);
  ASSERT_FALSE(d.HasParseError());

  const std::size_t before = dispatcher_->subscriptionCount();

  auto stranded = std::make_shared<Sink>();
  constexpr int kRejects = 25;
  int rejected = 0;
  for (int i = 0; i < kRejects; ++i) {
    try {
      auto chain = tree::buildForRequest(d, deps_, stranded);
      for (auto& n : chain.keepAlive) if (n) n->shutdown();
      if (chain.head) chain.head->shutdown();
    } catch (const std::exception&) { ++rejected; }
  }
  ASSERT_EQ(rejected, kRejects)
      << "this request must be REFUSED for the test to mean anything";

  EXPECT_EQ(dispatcher_->subscriptionCount(), before)
      << (dispatcher_->subscriptionCount() - before) << " subscription(s) "
         "survived " << kRejects << " builds that were all REFUSED.\n"
         "    The arity check must run BEFORE any node is constructed: the "
         "`Listener` builder registers\n"
         "    with the Dispatcher as it is constructed, and the only thing that "
         "unregisters is\n"
         "    `Listener::shutdown()`. A stranded subscription is walked on "
         "every onTick forever, with\n"
         "    nothing left holding a handle that could stop it, and it is "
         "reachable from client JSON in a\n"
         "    loop (SPEC section 1.5). It delivers NOTHING observable — its "
         "downstream is a weak_ptr to a\n"
         "    destroyed port — which is why this counts subscriptions rather "
         "than values.";

  for (int n = 0; n < 4; ++n) tick("AAPL", {{"ask", 1.0 + n}, {"bid", 2.0 + n}});
  pool_->drain();

  EXPECT_EQ(stranded->values().size(), 0u)
      << stranded->values().size() << " value(s) reached a terminal belonging "
         "to a build REFUSED " << kRejects << " times: "
      << render(stranded->values());
}

// The non-integer case. `sizeOr` gates on `IsUint()`, so `{"arity": 2.0}` and
// `{"arity": "2"}` fall through to the default and used to be reported as
// "positive 'arity' required" — true but misleading, because the author DID
// supply a positive arity and the real complaint is its JSON type. Nothing
// covered it (ENC-1291 adversarial pass).
TEST_F(FanInArity, NonIntegerArityIsRefusedForTheRightReason) {
  for (const char* body : {
         R"("arity":2.0)",
         R"("arity":"2")",
         R"("arity":-1)" }) {
    const std::string req =
      std::string(R"({"key":1,"streamKey":"AAPL","field":"lastPrice",)") +
      R"("node":{"type":"Aggregate",)" + body + R"(,"inputs":[)" +
      R"({"type":"Listener","streamKey":"AAPL","field":"ask"},)" +
      R"({"type":"Listener","streamKey":"AAPL","field":"bid"}]}})";
    rapidjson::Document d;
    d.Parse(req.c_str());
    ASSERT_FALSE(d.HasParseError()) << req;

    auto sink = std::make_shared<Sink>();
    std::string msg;
    try {
      tree::buildForRequest(d, deps_, sink);
      FAIL() << "accepted " << req;
    } catch (const std::exception& ex) { msg = ex.what(); }

    EXPECT_NE(msg.find("'arity' must be a non-negative whole number"),
              std::string::npos)
        << "for " << body << " the engine must say the arity's TYPE is wrong, "
           "not that it is missing. Got: " << msg;
  }
}

// ═══ 3. What the check costs the checked-in corpus ═══════════════════════════

TEST_F(FanInArity, EveryCorpusAggregateDeclaresArityEqualToItsInputCount) {
  rapidjson::Document& doc = corpusDoc();
  ASSERT_FALSE(doc.IsNull()) << "corpus_requests.json not found";
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());

  std::size_t entries = 0, aggregates = 0, mismatches = 0;
  std::string offenders;

  for (const auto& e : doc.GetArray()) {
    if (!e.IsObject() || !e.HasMember("request")) continue;
    ++entries;
    const int id = (e.HasMember("corpus_id") && e["corpus_id"].IsInt())
                     ? e["corpus_id"].GetInt() : -1;
    walkSpecs(e["request"], [&](const rapidjson::Value& spec) {
      if (std::string(spec["type"].GetString()) != "Aggregate") return;
      ++aggregates;
      const std::size_t declared =
        (spec.HasMember("arity") && spec["arity"].IsUint())
          ? spec["arity"].GetUint() : 0u;
      const std::size_t actual =
        (spec.HasMember("inputs") && spec["inputs"].IsArray())
          ? spec["inputs"].Size() : 0u;
      if (declared != actual) {
        ++mismatches;
        offenders += "\n      corpus_id " + std::to_string(id) +
                     ": arity=" + std::to_string(declared) +
                     " inputs=" + std::to_string(actual);
      }
    });
  }

  EXPECT_EQ(entries, 272u)
      << "the corpus changed size; re-measure what this check costs";
  EXPECT_EQ(aggregates, 52u)
      << "the corpus no longer carries 52 `Aggregate` specs, so a zero "
         "refusal count below would be measuring nothing";
  EXPECT_EQ(mismatches, 0u)
      << "ENC-1291's arity check REFUSES " << mismatches << " of " << entries
      << " checked-in corpus requests:" << offenders
      << "\n    That is a behaviour change to stored graphs and must be "
         "reported, not absorbed (SPEC D6).";
}
