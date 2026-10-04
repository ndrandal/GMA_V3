#include "gma/server/RequestCanonicalKey.hpp"

#include <algorithm>
#include <string_view>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace gma::server {

// ---------------------------------------------------------------------------
// The node-type classification.
//
// This table answers ONE question per type: "if a second subscriber attaches to
// a running DAG containing this node, can it observe a value sequence it could
// not have observed from its own freshly-built DAG?" It is NOT a judgement
// about whether the node is otherwise well-behaved.
//
// It is exhaustive by construction and a test enforces that: every name in
// `gma::engine::NodeTypeRegistry::names()` must appear below, so a node type
// registered without being classified turns
// `SharedSubscriptionTest.EveryRegisteredNodeTypeIsClassified` red rather than
// defaulting into either answer. `classifyNodeType` additionally fails CLOSED
// for anything absent, so even with the test deleted an unclassified type is
// refused rather than shared.
//
// WHY EACH SENSITIVE ONE IS SENSITIVE (measured, ENC-1041; file:line are this
// commit's):
//
//  * `Worker` — `acc_` (include/gma/nodes/Worker.hpp:57) retains up to
//    MAX_ACC=1000 values PER SYMBOL and `fn_` is applied to the whole retained
//    span (src/nodes/Worker.cpp:32), never to the arriving value alone. There
//    is no clear on any semantic boundary — `acc_` is emptied only in
//    `shutdown()` (:56), which the node's own header calls out as a
//    documented-but-unimplemented "since last clear"
//    (include/gma/nodes/Worker.hpp:22-27, SPEC
//    specs/2026-09-20-gma-join-correctness Q5). So an attacher's first value is
//    a reduction over up to 1000 PRE-ATTACH samples while a fresh Worker's
//    first value is a reduction over one. The divergence lasts until 1000 new
//    values flush the window, i.e. unbounded wall time on a slow stream. This
//    is the single most common node in the 272-entry corpus (161 occurrences),
//    so it is also the main reason the shareable fraction is what it is.
//  * `Pack` — `state_[sym].latest` (include/gma/nodes/Pack.hpp:70-87) is
//    combine-latest and is never reset after an emit (src/nodes/Pack.cpp:59-75).
//    A fresh Pack emits NOTHING until each of its N fields has fired once; a
//    warm one emits a complete Record on the first post-attach update, carrying
//    pre-attach values in the other N-1 slots — and a field that never updates
//    again keeps feeding its pre-attach value forever. Longest-lived divergence
//    of the seven.
//  * `Aggregate` — pending tuple slots (include/gma/nodes/Aggregate.hpp:144-165,
//    filled at src/nodes/Aggregate.cpp:112-113). Up to `arity_-1` slots can
//    already hold pre-attach values, so the first post-attach tuple completes
//    early and mixes them in. Self-heals after one completed tuple (the barrier
//    reset at :121-122) — bounded, but not identical.
//  * `Interval` — its tick phase is fixed by whenever `start()` ran, because
//    the wait is RELATIVE (`cv.wait_for(lk, st->period, …)`,
//    src/nodes/Interval.cpp:33). An attacher inherits the first subscriber's
//    phase; a fresh Interval would be silent for a full period and then tick on
//    a different offset. The pulse payload is a constant 0.0 (:48,:50), so what
//    diverges is the SAMPLING INSTANTS of everything it clocks — and unlike
//    `BucketTime` it stamps no `bucketStartMs`, so the shift is not even
//    recoverable by the client.
//  * `TumblingWindow` — wall-clock-aligned, so the GRID matches a fresh node
//    (src/nodes/TumblingWindow.cpp:76-77); but `State::acc`
//    (include/gma/nodes/TumblingWindow.hpp:98) already holds the current
//    partial bucket, so the first emitted vector spans the whole bucket where a
//    fresh node's spans only the post-attach remainder. One bucket of
//    divergence.
//  * `GroupSplit` / `SymbolSplit` — one type, two wire names
//    (src/core/TreeBuilder.cpp:1732-1733). `children_`
//    (include/gma/nodes/GroupSplit.hpp:25) memoizes one child SUBTREE per group
//    key, built on first sighting (src/nodes/GroupSplit.cpp:36) through a full
//    `tree::buildOne` (src/core/TreeBuilder.cpp:1718-1724). So every stateful
//    node inside a child is already warm at attach, and a child-side `Interval`
//    has its phase pinned to when the FIRST subscriber first saw that group.
//
// AND WHY THE OTHERS ARE NOT:
//
//  * `Listener` — registration only, no value history, and
//    `Dispatcher::registerListener` appends without replaying a last value
//    (src/core/Dispatcher.cpp:33-39), so a fresh Listener also starts cold.
//  * `AtomicAccessor` — reads `store_->get(symbol_, field_)` at the instant of
//    the incoming pulse (src/nodes/AtomicAccessor.cpp:23). The `AtomicStore` is
//    process-global and already shared by every DAG, so a fresh accessor reads
//    the same number. (Its sampling INSTANTS come from whatever clocks it,
//    which is why `Interval` above is classified on its own.)
//  * `BucketTime` — the only timer whose grid is independent of construction
//    time: every iteration recomputes the boundary from `system_clock::now()`
//    floor-aligned to the period (src/nodes/BucketTime.cpp:31-47,70-71), it
//    carries no cross-iteration state, and `onValue` is a no-op (:106-108).
//  * `VectorReducer`, `Field`, `Expr`, `Filter`, `Switch` — pure functions of
//    the one arriving value. `Expr`'s env is rebuilt per value
//    (src/nodes/Expr.cpp:22) and every operator closure captures only its args
//    (src/core/Expr.cpp:105-126).
//  * `Tee` — pure fan-out; its output set is fixed at construction.
//  * `Chain`, `Ref`, `Let` — BUILD-TIME ONLY. `Chain` returns its first stage's
//    head verbatim (src/core/TreeBuilder.cpp:1756), `Ref` returns a no-op stub
//    (:982-988), `Let` returns a `CompositeRoot` whose `onValue` only forwards
//    the clock tick (:852-864). None accumulates anything. They are classified
//    invariant for THEMSELVES, which is sound here because the walk below
//    visits every nested object in the document — so a `Let` binding's producer
//    or a `Chain` stage is classified on its own merits and a sensitive node
//    hiding inside one still refuses the whole request.
// ---------------------------------------------------------------------------
namespace {

const ClassifiedNodeType kTable[] = {
    // --- adds no attach sensitivity -------------------------------------
    {"Listener",       NodeShareClass::AttachInvariant},
    {"AtomicAccessor", NodeShareClass::AttachInvariant},
    {"BucketTime",     NodeShareClass::AttachInvariant},
    {"VectorReducer",  NodeShareClass::AttachInvariant},
    {"Field",          NodeShareClass::AttachInvariant},
    {"Expr",           NodeShareClass::AttachInvariant},
    {"Filter",         NodeShareClass::AttachInvariant},
    {"Switch",         NodeShareClass::AttachInvariant},
    {"Tee",            NodeShareClass::AttachInvariant},
    {"Chain",          NodeShareClass::AttachInvariant},
    {"Ref",            NodeShareClass::AttachInvariant},
    {"Let",            NodeShareClass::AttachInvariant},
    // --- attach-sensitive: refuse to share ------------------------------
    {"Worker",         NodeShareClass::AttachSensitive},
    {"Pack",           NodeShareClass::AttachSensitive},
    {"Aggregate",      NodeShareClass::AttachSensitive},
    {"Interval",       NodeShareClass::AttachSensitive},
    {"TumblingWindow", NodeShareClass::AttachSensitive},
    {"GroupSplit",     NodeShareClass::AttachSensitive},
    {"SymbolSplit",    NodeShareClass::AttachSensitive},
};

} // namespace

NodeShareClass classifyNodeType(std::string_view type) noexcept {
  for (const auto& e : kTable) {
    if (type == e.type) return e.cls;
  }
  return NodeShareClass::Unknown;   // fail closed — N6
}

const std::vector<ClassifiedNodeType>& classifiedNodeTypes() {
  static const std::vector<ClassifiedNodeType> v(std::begin(kTable), std::end(kTable));
  return v;
}

const char* shareRefusalName(ShareRefusal r) noexcept {
  switch (r) {
    case ShareRefusal::None:                return "none";
    case ShareRefusal::NotAnObject:         return "not_an_object";
    case ShareRefusal::DepthExceeded:       return "depth_exceeded";
    case ShareRefusal::MalformedType:       return "malformed_type";
    case ShareRefusal::AttachSensitiveNode: return "attach_sensitive_node";
    case ShareRefusal::UnclassifiedNode:    return "unclassified_node";
  }
  return "unknown";
}

namespace {

using CanonWriter = ::rapidjson::Writer<::rapidjson::StringBuffer>;

// ONE pass does both jobs: it writes the canonical bytes and it classifies
// every node type it meets. Deliberately not two walks — two walks can disagree
// about which subtrees they visited, and then the key says "same request" about
// a tree the classifier never looked at.
//
// Returns false on the first refusal, with `out.refusal` / `out.detail` set.
bool canonWalk(const ::rapidjson::Value& v, CanonWriter& w, int depth,
               CanonicalRequest& out) {
  if (depth > kMaxCanonicalDepth) {
    out.refusal = ShareRefusal::DepthExceeded;
    return false;
  }

  if (v.IsObject()) {
    // A node spec carries its type under "type" and `TreeBuilder::expectType`
    // (src/core/TreeBuilder.cpp:51-55) requires it to be a string, so this is
    // the same member the builder dispatches on. Objects WITHOUT a "type" are
    // not node specs and contribute no classification.
    //
    // Collecting from EVERY object rather than from a known list of
    // subtree-bearing member names ("child", "inputs", "outputs", "cases",
    // "bindings", "stages", "pipeline", …) is the point: such a list is a floor
    // that goes stale the moment a node type introduces a new member name, and
    // going stale here would mean silently SHARING a sensitive node. The cost
    // of the broader rule is that a non-node object that happens to carry a
    // `type` member is also classified — which can only add refusals, never
    // remove one.
    if (auto it = v.FindMember("type"); it != v.MemberEnd()) {
      if (!it->value.IsString()) {
        out.refusal = ShareRefusal::MalformedType;
        return false;
      }
      const std::string_view type{it->value.GetString(), it->value.GetStringLength()};
      switch (classifyNodeType(type)) {
        case NodeShareClass::AttachInvariant:
          break;
        case NodeShareClass::AttachSensitive:
          out.refusal = ShareRefusal::AttachSensitiveNode;
          out.detail.assign(type);
          return false;
        case NodeShareClass::Unknown:
          out.refusal = ShareRefusal::UnclassifiedNode;
          out.detail.assign(type);
          return false;
      }
    }

    // Stable sort by member name. STABLE matters: rapidjson keeps repeated
    // member names and resolves a lookup to the FIRST of them, so `{"a":1,"a":2}`
    // and `{"a":2,"a":1}` build different trees. A stable sort preserves their
    // relative order and therefore keeps those two documents apart; an unstable
    // one could collapse them and merge two requests that are not the same.
    std::vector<const ::rapidjson::Value::Member*> members;
    members.reserve(v.MemberCount());
    for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m) members.push_back(&*m);
    std::stable_sort(members.begin(), members.end(),
                     [](const auto* a, const auto* b) {
                       return std::string_view(a->name.GetString(),
                                               a->name.GetStringLength()) <
                              std::string_view(b->name.GetString(),
                                               b->name.GetStringLength());
                     });

    w.StartObject();
    for (const auto* m : members) {
      w.Key(m->name.GetString(), m->name.GetStringLength());
      if (!canonWalk(m->value, w, depth + 1, out)) return false;
    }
    w.EndObject();
    return true;
  }

  if (v.IsArray()) {
    // Order preserved: `pipeline`/`stages` is an ordered chain and a fan-in's
    // `inputs` is positional. No commutativity normalisation, even where the
    // reducer happens to be commutative.
    w.StartArray();
    for (const auto& e : v.GetArray()) {
      if (!canonWalk(e, w, depth + 1, out)) return false;
    }
    w.EndArray();
    return true;
  }

  // Scalars: rapidjson's own rendering of the type the value parsed to. `20`
  // and `20.0` therefore stay distinct (N1) — a missed merge, never a wrong one.
  v.Accept(w);
  return true;
}

} // namespace

CanonicalRequest canonicalizeRequest(const ::rapidjson::Value& rq) noexcept {
  CanonicalRequest out;
  if (!rq.IsObject()) {
    out.refusal = ShareRefusal::NotAnObject;
    return out;
  }
  try {
    ::rapidjson::StringBuffer sb;
    CanonWriter w(sb);
    if (!canonWalk(rq, w, 0, out)) {
      out.key.clear();
      return out;
    }
    out.key.assign(sb.GetString(), sb.GetSize());
  } catch (...) {
    // Never let canonicalization decide whether a request is SERVED. Anything
    // unexpected degrades to "do not share", which is the pre-dedup behaviour.
    out.key.clear();
    out.refusal = ShareRefusal::NotAnObject;
  }
  return out;
}

} // namespace gma::server
