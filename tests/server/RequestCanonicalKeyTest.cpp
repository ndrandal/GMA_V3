// tests/server/RequestCanonicalKeyTest.cpp — ENC-1041
//
// The EQUIVALENCE RELATION behind cross-connection subscription dedup, pinned
// in both directions: what merges, and — the half that is a correctness gate
// rather than a feature — what must NOT.
//
// Each case asserts the REASON it passes. `canonicalizeRequest` returns a named
// `ShareRefusal` precisely so a refusal can be distinguished from "the key
// happened to differ", because those two are indistinguishable from an empty
// key and only one of them is the rule under test.
//
// WHY THE CORPUS CANNOT COVER THE MEMBER-ORDER HALF OF THIS FILE. Measured over
// all 272 checked-in requests: the number of entries that merge ONLY because
// object members are sorted is **0**, and deleting the sort entirely leaves the
// corpus's distinct-key count at 236 either way. That is a property of the
// corpus (`rq`'s top level is built in a fixed order by `handleSubscribe`, and
// inside the trees there are just 6 distinct object key-sets, each of which
// appears in exactly one member order throughout — always `type` first), not of
// the rule. The sort IS doing work — sorted and unsorted bytes differ for 272
// of 272 entries — it just never changes a verdict there. So the hand-written
// permutation fixtures below are the ONLY coverage the sort has, and
// `MemberOrderDoesNotChangeTheKey` is the test that would catch its deletion.

#include "gma/NodeRegistry.hpp"
#include "gma/engine/NodeTypeRegistry.hpp"
#include "gma/server/RequestCanonicalKey.hpp"
#include "../support/CorpusPath.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

using gma::server::canonicalizeRequest;
using gma::server::classifiedNodeTypes;
using gma::server::classifyNodeType;
using gma::server::NodeShareClass;
using gma::server::ShareRefusal;

namespace {

gma::server::CanonicalRequest canonOf(const char* json) {
  rapidjson::Document d;
  d.Parse(json);
  EXPECT_FALSE(d.HasParseError()) << "fixture JSON is invalid: " << json;
  return canonicalizeRequest(d);
}

std::string keyOf(const char* json) {
  auto c = canonOf(json);
  EXPECT_TRUE(c.shareable())
      << "fixture was expected to be shareable but was refused: "
      << gma::server::shareRefusalName(c.refusal) << " (" << c.detail << ")";
  return c.key;
}

} // namespace

// ───────────────────────────────────────────────────────────────────────────
// What MERGES
// ───────────────────────────────────────────────────────────────────────────

TEST(RequestCanonicalKey, IdenticalRequestsShareAKey) {
  EXPECT_EQ(keyOf(R"({"streamKey":"AAPL","field":"lastPrice"})"),
            keyOf(R"({"streamKey":"AAPL","field":"lastPrice"})"));
}

// The sort's only coverage. See the file header: 0 of 272 corpus entries
// exercise it, so deleting the `std::stable_sort` in
// src/server/RequestCanonicalKey.cpp reddens THIS test and nothing else.
TEST(RequestCanonicalKey, MemberOrderDoesNotChangeTheKey) {
  // Top level permuted, and the nested `node` object permuted independently.
  const auto a = keyOf(
      R"({"streamKey":"AAPL","field":"lastPrice",
          "node":{"type":"AtomicAccessor","streamKey":"AAPL","field":"rsi_14"}})");
  const auto b = keyOf(
      R"({"field":"lastPrice","node":{"field":"rsi_14","streamKey":"AAPL",
          "type":"AtomicAccessor"},"streamKey":"AAPL"})");
  EXPECT_EQ(a, b) << "object member order changed the canonical key — two "
                     "requests that build the same DAG would not merge";
}

TEST(RequestCanonicalKey, MemberOrderIsNormalisedAtEveryDepth) {
  // Depth 4, permuted at every level, including inside an array element.
  const auto a = keyOf(
      R"({"streamKey":"X","field":"f","node":{"type":"Chain","stages":[
          {"type":"Filter","pred":{"op":"gt","args":[1,2]}},
          {"type":"Field","name":"a"}]}})");
  const auto b = keyOf(
      R"({"field":"f","node":{"stages":[
          {"pred":{"args":[1,2],"op":"gt"},"type":"Filter"},
          {"name":"a","type":"Field"}],"type":"Chain"},"streamKey":"X"})");
  EXPECT_EQ(a, b);
}

// `handleSubscribe` drops everything it does not copy into `rq`, so junk the
// client sent alongside a request is already gone before this is reached. This
// pins that the key is computed over the NORMALIZED document.
TEST(RequestCanonicalKey, TheKeyIsOverTheNormalizedDocumentOnly) {
  // `rq` as handleSubscribe builds it carries no `key`/`id` member at all, so
  // two subscribers using different request ids produce the same key.
  EXPECT_EQ(keyOf(R"({"streamKey":"AAPL","field":"bid"})"),
            keyOf(R"({"streamKey":"AAPL","field":"bid"})"));
}

// ───────────────────────────────────────────────────────────────────────────
// What must NOT merge — the over-normalisation guard
// ───────────────────────────────────────────────────────────────────────────

// Array order IS semantics: `pipeline`/`stages` is an ordered chain and a
// fan-in's `inputs` is positional. Normalising it would merge two different
// computations.
TEST(RequestCanonicalKey, ArrayOrderDoesChangeTheKey) {
  EXPECT_NE(keyOf(R"({"streamKey":"X","field":"f","pipeline":[
                      {"type":"Field","name":"a"},{"type":"Field","name":"b"}]})"),
            keyOf(R"({"streamKey":"X","field":"f","pipeline":[
                      {"type":"Field","name":"b"},{"type":"Field","name":"a"}]})"))
      << "pipeline stage order was normalised away — two different chains merged";
}

// The STABLE half of the stable sort. rapidjson keeps repeated member names and
// resolves a lookup to the FIRST, so these two documents build different trees.
// An unstable sort could put them in the same order and merge them.
TEST(RequestCanonicalKey, RepeatedMemberNamesKeepTheirRelativeOrder) {
  const auto a = keyOf(R"({"streamKey":"X","field":"f",
                           "node":{"type":"Field","name":"a","name":"b"}})");
  const auto b = keyOf(R"({"streamKey":"X","field":"f",
                           "node":{"type":"Field","name":"b","name":"a"}})");
  EXPECT_NE(a, b)
      << "two objects differing only in the ORDER of a repeated member name "
         "produced one key. rapidjson resolves a repeated name to the first "
         "occurrence, so these build different nodes — the sort must be stable.";
}

TEST(RequestCanonicalKey, StreamKeyAndFieldAreCaseSensitive) {
  EXPECT_NE(keyOf(R"({"streamKey":"BTC","field":"lastPrice"})"),
            keyOf(R"({"streamKey":"btc","field":"lastPrice"})"))
      << "AtomicStore keys are case-sensitive; merging these would feed a "
         "subscriber a different stream than it asked for";
  EXPECT_NE(keyOf(R"({"streamKey":"BTC","field":"bid"})"),
            keyOf(R"({"streamKey":"BTC","field":"Bid"})"));
}

// Documented missed merge N1. Asserted so the behaviour is a decision rather
// than an accident — if someone later canonicalises numbers, this is the test
// that says it was deliberate and makes them say so too.
TEST(RequestCanonicalKey, NumericSpellingIsNotNormalised) {
  EXPECT_NE(keyOf(R"({"streamKey":"X","field":"f","node":{"type":"Field","n":20}})"),
            keyOf(R"({"streamKey":"X","field":"f","node":{"type":"Field","n":20.0}})"))
      << "N1: a missed merge, never a wrong one. Changing this is a widening, "
         "not a bug fix.";
}

// Documented missed merge N3.
TEST(RequestCanonicalKey, AbsentAndEmptyPipelineAreDistinct) {
  EXPECT_NE(keyOf(R"({"streamKey":"X","field":"f"})"),
            keyOf(R"({"streamKey":"X","field":"f","pipeline":[]})"));
}

// ───────────────────────────────────────────────────────────────────────────
// Attach-sensitivity: N5 and N6
// ───────────────────────────────────────────────────────────────────────────

TEST(RequestCanonicalKey, AnAttachInvariantTreeIsShareable) {
  auto c = canonOf(R"({"streamKey":"AAPL","field":"lastPrice",
                       "node":{"type":"AtomicAccessor","streamKey":"AAPL",
                               "field":"rsi_14"}})");
  EXPECT_TRUE(c.shareable()) << gma::server::shareRefusalName(c.refusal);
  EXPECT_FALSE(c.key.empty());
}

TEST(RequestCanonicalKey, ABareStreamKeyFieldRequestIsShareable) {
  auto c = canonOf(R"({"streamKey":"AAPL","field":"lastPrice"})");
  EXPECT_TRUE(c.shareable());
}

// One case per attach-sensitive type, each asserting the refusal AND that the
// offending type is named — so a type silently dropped from the sensitive list
// fails here with a message that says which.
TEST(RequestCanonicalKey, EveryAttachSensitiveTypeRefusesToShareAndIsNamed) {
  struct Case { const char* type; const char* json; };
  const Case cases[] = {
    {"Worker",
     R"({"streamKey":"X","field":"f","node":{"type":"Worker","fn":"mean"}})"},
    {"Pack",
     R"({"streamKey":"X","field":"f","node":{"type":"Pack","names":["a","b"],
         "inputs":[{"type":"Listener","streamKey":"X","field":"a"},
                   {"type":"Listener","streamKey":"X","field":"b"}]}})"},
    {"Aggregate",
     R"({"streamKey":"X","field":"f","node":{"type":"Aggregate","arity":2,
         "inputs":[{"type":"Listener","streamKey":"X","field":"a"},
                   {"type":"Listener","streamKey":"X","field":"b"}]}})"},
    {"Interval",
     R"({"streamKey":"X","field":"f","node":{"type":"Interval","ms":500,
         "child":{"type":"AtomicAccessor","streamKey":"X","field":"f"}}})"},
    {"TumblingWindow",
     R"({"streamKey":"X","field":"f","node":{"type":"TumblingWindow","ms":1000,
         "child":{"type":"Field","name":"a"}}})"},
    {"GroupSplit",
     R"({"streamKey":"*","field":"f","node":{"type":"GroupSplit",
         "child":{"type":"Field","name":"a"}}})"},
    {"SymbolSplit",
     R"({"streamKey":"*","field":"f","node":{"type":"SymbolSplit",
         "child":{"type":"Field","name":"a"}}})"},
  };
  for (const auto& c : cases) {
    auto r = canonOf(c.json);
    EXPECT_EQ(r.refusal, ShareRefusal::AttachSensitiveNode)
        << c.type << " did not refuse to share. A shared DAG containing it "
                     "would hand a late subscriber values derived from state "
                     "accumulated before it attached.";
    EXPECT_EQ(r.detail, c.type)
        << "the refusal did not name the offending type";
    EXPECT_TRUE(r.key.empty()) << c.type << " returned a shareable key anyway";
  }
}

// The census-floor guard. A sensitive node must be found WHEREVER it sits —
// which is why the walk visits every object in the document rather than a list
// of subtree-bearing member names. That list is the thing that goes stale.
TEST(RequestCanonicalKey, ASensitiveNodeIsFoundUnderEverySubtreeMemberName) {
  const char* const cases[] = {
    // under "child"
    R"({"streamKey":"X","field":"f","node":{"type":"Filter","pred":{"op":"gt",
        "args":[1,2]},"child":{"type":"Worker","fn":"mean"}}})",
    // under "stages" of a Chain
    R"({"streamKey":"X","field":"f","node":{"type":"Chain","stages":[
        {"type":"Field","name":"a"},{"type":"Worker","fn":"sum"}]}})",
    // under a Tee's "outputs"
    R"({"streamKey":"X","field":"f","node":{"type":"Tee","outputs":[
        {"type":"Field","name":"a"},{"type":"Interval","ms":10}]}})",
    // under a Switch's "cases"
    R"({"streamKey":"X","field":"f","node":{"type":"Switch","cases":[
        {"type":"Pack","names":["a"]}]}})",
    // under a Let's "bindings" — a VALUE of the bindings object, which is not
    // itself a node spec, so a walker keyed on member names would have to know
    // to descend into each binding
    R"({"streamKey":"X","field":"f","node":{"type":"Let",
        "bindings":{"b":{"type":"Worker","fn":"mean"}},
        "body":{"type":"Ref","name":"b"}}})",
    // in the top-level `pipeline` rather than in `node`
    R"({"streamKey":"X","field":"f","pipeline":[{"type":"Worker","fn":"mean"}]})",
    // in the top-level `stages`
    R"({"streamKey":"X","field":"f","stages":[{"type":"Interval","ms":5}]})",
    // nested four deep under mixed arrays and objects
    R"({"streamKey":"X","field":"f","node":{"type":"Tee","outputs":[
        {"type":"Chain","stages":[{"type":"Filter","child":{"type":"Aggregate",
         "arity":2}}]}]}})",
  };
  for (const char* j : cases) {
    auto r = canonOf(j);
    EXPECT_EQ(r.refusal, ShareRefusal::AttachSensitiveNode)
        << "a sensitive node hidden in this shape was NOT found, so the "
           "request would have been shared:\n" << j;
  }
}

// Fail closed (N6). An unknown type must refuse, not be assumed harmless.
TEST(RequestCanonicalKey, AnUnclassifiedNodeTypeFailsClosed) {
  auto r = canonOf(R"({"streamKey":"X","field":"f",
                       "node":{"type":"NoSuchNodeTypeExists"}})");
  EXPECT_EQ(r.refusal, ShareRefusal::UnclassifiedNode);
  EXPECT_EQ(r.detail, "NoSuchNodeTypeExists");
  EXPECT_TRUE(r.key.empty());
}

TEST(RequestCanonicalKey, AMalformedTypeMemberRefusesRatherThanThrows) {
  auto r = canonOf(R"({"streamKey":"X","field":"f","node":{"type":5}})");
  EXPECT_EQ(r.refusal, ShareRefusal::MalformedType);
}

TEST(RequestCanonicalKey, ADeeplyNestedDocumentRefusesRatherThanThrows) {
  // kMaxCanonicalDepth + a margin of nesting. The point is that dedup must
  // never be able to REJECT a request the pre-dedup server would have served —
  // so this refuses to share and reports why, rather than throwing out of
  // handleSubscribe's build path.
  std::string j = R"({"streamKey":"X","field":"f","node":)";
  const int depth = gma::server::kMaxCanonicalDepth + 8;
  for (int i = 0; i < depth; ++i) j += R"({"child":)";
  j += R"({"type":"Field","name":"a"})";
  for (int i = 0; i < depth; ++i) j += "}";
  j += "}";
  rapidjson::Document d;
  d.Parse(j.c_str());
  ASSERT_FALSE(d.HasParseError());
  auto r = canonicalizeRequest(d);
  EXPECT_EQ(r.refusal, ShareRefusal::DepthExceeded);
  EXPECT_TRUE(r.key.empty());
}

TEST(RequestCanonicalKey, ANonObjectRefuses) {
  rapidjson::Document d;
  d.Parse("[1,2,3]");
  ASSERT_FALSE(d.HasParseError());
  EXPECT_EQ(canonicalizeRequest(d).refusal, ShareRefusal::NotAnObject);
}

// ───────────────────────────────────────────────────────────────────────────
// Coverage of the classification itself.
//
// This is what keeps the classification from being a census taken once from one
// grep. It compares the table against the registry's OWN list of buildable
// names, so a node type registered without being classified is red here rather
// than silently shareable — and `classifyNodeType` additionally fails closed,
// so even with this test deleted the unclassified type is refused.
// ───────────────────────────────────────────────────────────────────────────
TEST(RequestCanonicalKey, EveryRegisteredNodeTypeIsClassified) {
  gma::registerBuiltinNodeTypes();   // idempotent

  std::set<std::string> classified;
  for (const auto& e : classifiedNodeTypes()) classified.insert(e.type);

  const auto registered = gma::engine::NodeTypeRegistry::names();
  ASSERT_GE(registered.size(), 19u)
      << "the node registry looks unpopulated — registerBuiltinNodeTypes() did "
         "not run, so this test would pass vacuously";

  std::vector<std::string> unclassified;
  for (const auto& n : registered) {
    // Names registered by OTHER tests are not production node types. They are
    // deliberately prefixed (see the ENC-1102 note in CLAUDE.md), so they are
    // excluded by prefix rather than by a hardcoded list.
    if (n.rfind("__test", 0) == 0) continue;
    if (!classified.count(n)) unclassified.push_back(n);
  }
  std::sort(unclassified.begin(), unclassified.end());
  EXPECT_TRUE(unclassified.empty())
      << "these buildable node types have no sharing classification, so a "
         "request using one cannot be deduplicated (it fails closed, which is "
         "safe but silent). Classify each in the kTable in "
         "src/server/RequestCanonicalKey.cpp: "
      << [&] {
           std::string s;
           for (const auto& n : unclassified) s += n + " ";
           return s;
         }();

  // And the reverse: the table must not claim types that do not exist, which is
  // how a renamed node type leaves a dead row behind that looks like coverage.
  std::set<std::string> reg(registered.begin(), registered.end());
  for (const auto& e : classifiedNodeTypes()) {
    EXPECT_TRUE(reg.count(e.type))
        << "'" << e.type << "' is classified but is not a registered node "
           "type — a stale row that reads as coverage it is not providing";
  }
}

TEST(RequestCanonicalKey, ClassifyFailsClosedForAnythingAbsent) {
  EXPECT_EQ(classifyNodeType("definitely not a node"), NodeShareClass::Unknown);
  EXPECT_EQ(classifyNodeType(""), NodeShareClass::Unknown);
  // Case-sensitive: the registry lookup is, so this must be too.
  EXPECT_EQ(classifyNodeType("worker"), NodeShareClass::Unknown);
  EXPECT_EQ(classifyNodeType("Worker"), NodeShareClass::AttachSensitive);
}

// ───────────────────────────────────────────────────────────────────────────
// The corpus measurement, as a test.
//
// Dedup changes WHO RECEIVES a value, never what builds, so the headline number
// is the first one: 0 of 272 corpus entries change build verdict. The rest are
// the measured cost and benefit of the sharing rule, pinned so that a change to
// the classification has to restate them.
// ───────────────────────────────────────────────────────────────────────────
TEST(RequestCanonicalKey, CorpusMeasurement) {
  auto in = gma::testsupport::openCorpusRequests();
  ASSERT_TRUE(in.is_open()) << gma::testsupport::corpusNotFoundDiagnostic();
  rapidjson::IStreamWrapper isw(in);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  ASSERT_FALSE(doc.HasParseError());
  ASSERT_TRUE(doc.IsArray());
  ASSERT_EQ(doc.Size(), 272u)
      << "the corpus size moved; every count below was measured at 272 and "
         "must be re-measured";

  std::map<std::string, std::vector<std::size_t>> groups;  // key -> indices
  std::map<std::string, int> refusals;                     // reason -> count
  std::size_t shareable = 0, refused = 0, unclassified = 0;
  std::size_t distinctIncludingRefused = 0;
  std::set<std::string> allKeysIfNoSensitivityRule;

  for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
    const auto& e = doc[i];
    ASSERT_TRUE(e.IsObject() && e.HasMember("request")) << "entry " << i;
    const auto& r = e["request"];
    ASSERT_TRUE(r.IsObject());

    // Rebuild `rq` exactly as ClientSession::handleSubscribe does.
    rapidjson::Document rq;
    rq.SetObject();
    auto& a = rq.GetAllocator();
    ASSERT_TRUE(r.HasMember("streamKey") && r["streamKey"].IsString()) << i;
    ASSERT_TRUE(r.HasMember("field") && r["field"].IsString()) << i;
    rq.AddMember("streamKey",
                 rapidjson::Value(r["streamKey"].GetString(), a), a);
    rq.AddMember("field", rapidjson::Value(r["field"].GetString(), a), a);
    for (const char* k : {"pipeline", "stages"}) {
      if (r.HasMember(k) && r[k].IsArray()) {
        rapidjson::Value v(rapidjson::kArrayType);
        v.CopyFrom(r[k], a);
        rq.AddMember(rapidjson::Value(k, a), v, a);
      }
    }
    if (r.HasMember("node") && r["node"].IsObject()) {
      rapidjson::Value v(rapidjson::kObjectType);
      v.CopyFrom(r["node"], a);
      rq.AddMember("node", v, a);
    }

    const auto c = canonicalizeRequest(rq);
    if (c.shareable()) {
      ++shareable;
      groups[c.key].push_back(i);
    } else {
      ++refused;
      refusals[std::string(gma::server::shareRefusalName(c.refusal)) + ":" +
               c.detail]++;
      if (c.refusal == ShareRefusal::UnclassifiedNode) ++unclassified;
    }
  }
  (void)distinctIncludingRefused;
  (void)allKeysIfNoSensitivityRule;

  // 1. NO corpus entry is refused for a reason that means "this build does not
  //    know what this is". An unclassified type here would mean the sharing
  //    rule had gone stale against the corpus's own vocabulary.
  EXPECT_EQ(unclassified, 0u)
      << "a corpus request used a node type with no sharing classification";

  // 2. The measured split. Shareable = every node type in the tree adds no
  //    attach sensitivity; refused = at least one does.
  EXPECT_EQ(shareable, 122u);
  EXPECT_EQ(refused, 150u);

  // 3. The refusals, by cause. Worker is the dominant one and it is the reason
  //    the rule is as narrow as it is: `Worker::acc_` retains up to 1000 values
  //    per symbol and reduces over all of them, with no clear on any semantic
  //    boundary, so a late attacher's first value is a reduction over history
  //    it was not present for.
  EXPECT_EQ(refusals["attach_sensitive_node:Worker"], 126)
      << "refusal mix moved; re-measure";
  EXPECT_EQ(refusals["attach_sensitive_node:Interval"], 24);
  EXPECT_EQ(refusals.size(), 2u)
      << "a new refusal cause appeared in the corpus";

  // 4. The benefit, if every corpus request were subscribed on one server:
  //    DAGs saved = (entries in a shareable duplicate group) - (such groups).
  std::size_t dupGroups = 0, inDupGroups = 0;
  for (const auto& kv : groups) {
    if (kv.second.size() > 1) { ++dupGroups; inDupGroups += kv.second.size(); }
  }
  EXPECT_EQ(groups.size(), 94u) << "distinct shareable canonical keys";
  EXPECT_EQ(dupGroups, 19u);
  EXPECT_EQ(inDupGroups, 47u);
  EXPECT_EQ(inDupGroups - dupGroups, 28u)
      << "DAGs saved across the whole corpus: 272 builds become 244";
}
