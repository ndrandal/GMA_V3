#include "gma/TreeBuilder.hpp"
#include "gma/NodeRegistry.hpp"

#include <functional>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <numeric>
#include <cmath>
#include <limits>

// Node types
#include "gma/nodes/Listener.hpp"
#include "gma/nodes/Aggregate.hpp"
#include "gma/nodes/GroupSplit.hpp"
#include "gma/nodes/Worker.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/Interval.hpp"
#include "gma/nodes/BucketTime.hpp"
#include "gma/nodes/TumblingWindow.hpp"
#include "gma/nodes/VectorReducer.hpp"
#include "gma/nodes/Tee.hpp"
#include "gma/nodes/InputPort.hpp"
#include "gma/nodes/Pack.hpp"
#include "gma/nodes/Field.hpp"
#include "gma/nodes/Expr.hpp"
#include "gma/nodes/Filter.hpp"
#include "gma/nodes/Switch.hpp"

// Runtime deps
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/engine/NodeTypeRegistry.hpp"


//
// ---------- Tiny JSON helpers (anon namespace) ----------
//
namespace {

inline const rapidjson::Value& expectObj(const rapidjson::Value& v, const char* what) {
  if (!v.IsObject())
    throw std::runtime_error(std::string("TreeBuilder: expected object for ") + what);
  return v;
}

inline const char* expectType(const rapidjson::Value& v) {
  if (!v.HasMember("type") || !v["type"].IsString())
    throw std::runtime_error("TreeBuilder: node missing 'type'");
  return v["type"].GetString();
}

inline std::string strOr(const rapidjson::Value& v,
                         const char*             k,
                         const std::string&      def = "") {
  return (v.HasMember(k) && v[k].IsString())
           ? std::string(v[k].GetString())
           : def;
}

inline int intOr(const rapidjson::Value& v, const char* k, int def) {
  return (v.HasMember(k) && v[k].IsInt()) ? v[k].GetInt() : def;
}

inline std::size_t sizeOr(const rapidjson::Value& v, const char* k, std::size_t def) {
  return (v.HasMember(k) && v[k].IsUint())
           ? static_cast<std::size_t>(v[k].GetUint())
           : def;
}

inline bool has(const rapidjson::Value& v, const char* k) {
  return v.HasMember(k);
}

} // namespace

//
// ---------- ENC-1293: Record-valued terminal rejection ----------
//
// SPEC specs/2026-09-20-gma-join-correctness D7.
//
// **THIS RESTRICTION IS DELIBERATELY TEMPORARY. ENC-1295 (embassy) lifts it.**
// Delete this whole block, its call site in `buildForRequest`, and
// `tests/treebuilder/RecordTerminalTest.cpp` once embassy consumes a
// Record-valued update instead of dropping it.
//
// WHY IT EXISTS. `Pack` emits a `gma::Record`; `util::writeArgTypeJson`
// (include/gma/util/JsonUtil.hpp) serializes a Record as a JSON **object** onto
// the live update frame (`src/server/ClientSession.cpp`'s `sendFn`). embassy's
// inbound path is scalar-only: `asFloat32` (embassy/internal/gma/types.go)
// decodes `number | string | boolean` and returns false for an object, and its
// caller in `internal/gma/client.go` does `if !ok { return }` — **no log, no
// metric, no error**, and the whole downstream contract is
// `ValueHandler func(value float32)`. So a `Pack`-terminated pipeline renders
// an empty chart with no diagnostic anywhere **in two repos**. Rejecting at
// build time converts that silent cross-repo failure into a loud single-repo
// one, on a frame the client already understands.
//
// WHAT IT IS, PRECISELY. A conservative static shape analysis of the request
// JSON, run BEFORE a single node is constructed (Listener/Interval/BucketTime
// all spawn threads in their builders, so rejecting afterwards would leak
// work). It answers one question: *is the value arriving at the terminal
// statically certain to be a Record?* Certain — never "probably". A false
// positive would refuse a request that works today; a false negative only
// leaves the pre-existing silent drop in place, which is the strictly safer
// direction while the restriction is temporary.
//
// Measured against the checked-in corpus at the commit that introduced this:
// **0 of 272 `tests/treebuilder/corpus_requests.json` entries are refused** —
// the corpus uses only Worker / AtomicAccessor / Listener / Aggregate /
// Interval / SymbolSplit, and constructs no Record at all.
//
// KNOWN CONSERVATIVE GAPS, recorded rather than hidden. Each of these emits a
// Record the analysis calls `Opaque`, so the silent drop survives there:
//   * `Field` projecting a nested Record out of a `Pack{a: Pack{...}}`.
//   * `Worker` / `Expr` / `AtomicAccessor` / `Listener`, whose runtime value
//     is a feed- or function-supplied `ArgType` this analysis cannot see.
// None is reachable from anything the corpus or forum authors today, and all
// of them stop mattering when ENC-1295 lands.
//
namespace {

// What a node emits into its `downstream`.
//   Record — statically certain to be a `gma::Record`.
//   Opaque — everything else (scalar, vector, or not knowable here).
enum class ValueShape { Opaque, Record };

// Build-time `Let` scope, so a `Ref` in terminal position resolves to the
// producer that will actually feed the terminal.
struct ShapeScope {
  const rapidjson::Value* bindings { nullptr };
  const ShapeScope*       parent   { nullptr };
};

// Mirrors JsonValidator::MAX_TREE_DEPTH's role: a guard, not a semantic limit.
// Exceeding it yields Opaque — buildOne still runs and reports the real error.
constexpr int kMaxShapeDepth = 64;

ValueShape shapeInto(const rapidjson::Value& spec,
                     ValueShape              upstream,
                     const ShapeScope*       scope,
                     int                     depth,
                     std::string*            culprit) {
  if (depth > kMaxShapeDepth) return ValueShape::Opaque;
  if (!spec.IsObject() || !spec.HasMember("type") || !spec["type"].IsString())
    return ValueShape::Opaque;            // malformed — buildOne throws its own error

  const std::string type = spec["type"].GetString();

  auto record = [&](const char* t) {
    if (culprit && culprit->empty()) *culprit = t;
    return ValueShape::Record;
  };

  // The only node that CONSTRUCTS a Record (src/nodes/Pack.cpp).
  if (type == "Pack") return record("Pack");

  // Pass-through: forwards the value it received, unchanged
  // (src/nodes/Filter.cpp `ds->onValue(sv)`).
  if (type == "Filter") return upstream;

  // Aggregate re-emits its buffered INPUT values one at a time
  // (src/nodes/Aggregate.cpp), so its shape is its inputs' shape. Its inputs
  // are built terminating in the Aggregate, whose own upstream is this node's.
  if (type == "Aggregate") {
    ValueShape out = ValueShape::Opaque;
    if (spec.HasMember("inputs") && spec["inputs"].IsArray())
      for (const auto& in : spec["inputs"].GetArray())
        if (shapeInto(in, upstream, scope, depth + 1, culprit) == ValueShape::Record)
          out = ValueShape::Record;
    return out;
  }

  // Chain threads its stages upstream -> downstream; the last one feeds
  // `downstream`, so thread the shape through in the same order.
  if (type == "Chain") {
    ValueShape cur = upstream;
    if (spec.HasMember("stages") && spec["stages"].IsArray())
      for (const auto& st : spec["stages"].GetArray())
        cur = shapeInto(st, cur, scope, depth + 1, culprit);
    return cur;
  }

  // Tee and Switch build EVERY branch terminating in the shared `downstream`,
  // so one Record-valued branch is enough to poison the terminal.
  if (type == "Tee" || type == "Switch") {
    const char* key = (type == "Tee") ? "outputs" : "cases";
    ValueShape out = ValueShape::Opaque;
    if (spec.HasMember(key) && spec[key].IsArray())
      for (const auto& b : spec[key].GetArray())
        if (shapeInto(b, upstream, scope, depth + 1, culprit) == ValueShape::Record)
          out = ValueShape::Record;
    if (type == "Switch" && spec.HasMember("default") &&
        shapeInto(spec["default"], upstream, scope, depth + 1, culprit)
          == ValueShape::Record)
      out = ValueShape::Record;
    return out;
  }

  // GroupSplit builds its child per group, terminating in `downstream`.
  if (type == "GroupSplit" || type == "SymbolSplit")
    return spec.HasMember("child")
             ? shapeInto(spec["child"], upstream, scope, depth + 1, culprit)
             : ValueShape::Opaque;

  // Both timers tick `StreamValue{"", 0.0}` into their child
  // (src/nodes/Interval.cpp `timerLoop`), so the child's upstream is a scalar
  // regardless of what reached the timer.
  if (type == "Interval" || type == "BucketTime")
    return spec.HasMember("child")
             ? shapeInto(spec["child"], ValueShape::Opaque, scope, depth + 1, culprit)
             : ValueShape::Opaque;

  // Let builds its body with `downstream`; a Ref sitting in terminal position
  // is fed by its binding's producer, so resolve through the scope chain.
  if (type == "Let") {
    ShapeScope s;
    s.bindings = (spec.HasMember("bindings") && spec["bindings"].IsObject())
                   ? &spec["bindings"] : nullptr;
    s.parent   = scope;
    return spec.HasMember("body")
             ? shapeInto(spec["body"], upstream, &s, depth + 1, culprit)
             : ValueShape::Opaque;
  }

  if (type == "Ref") {
    const std::string name = strOr(spec, "name", "");
    if (name.empty()) return ValueShape::Opaque;
    for (const ShapeScope* s = scope; s; s = s->parent) {
      if (!s->bindings || !s->bindings->HasMember(name.c_str())) continue;
      // Producers see the OUTER scope only (see the "Let" builder's comment),
      // so resolve the producer against `s->parent`, not `s`.
      return shapeInto((*s->bindings)[name.c_str()], ValueShape::Opaque,
                       s->parent, depth + 1, culprit);
    }
    return ValueShape::Opaque;
  }

  // Everything else emits a scalar or a vector:
  //   Listener / AtomicAccessor  — a pushed or stored feed value
  //   Worker / Expr / VectorReducer — a computed number
  //   TumblingWindow             — vector<double>
  //   Field                      — one field PROJECTED OUT of a Record, which
  //                                is exactly the reduction this rejection
  //                                tells the author to apply
  return ValueShape::Opaque;
}

std::string recordTerminalMessage(const std::string& culprit) {
  return "buildForRequest: this request's terminal would receive a Record value"
         " (produced by node type '" + (culprit.empty() ? std::string("Pack") : culprit) +
         "'), which GMA serializes as a JSON object. That is rejected at build time"
         " because embassy's inbound path is scalar-only — asFloat32 decodes"
         " number|string|boolean and its caller drops anything else with no log, no"
         " metric and no error — so this request would render an EMPTY CHART with no"
         " diagnostic in either repo. Reduce the Record to a scalar before the"
         " terminal: the canonical shapes are Pack -> Expr -> Responder and"
         " Pack -> Field -> Responder. This restriction is TEMPORARY (ENC-1293) and"
         " is lifted by ENC-1295 once embassy consumes a Record; see"
         " specs/2026-09-20-gma-join-correctness/SPEC.md D7.";
}

} // namespace

//
// ---------- ENC-1336: a fan-in as a pipeline stage with something upstream ----------
//
// SPEC specs/2026-09-20-gma-join-correctness/SPEC.md §5 Q7 (ruled 2026-09-20 by
// ENC-1334), D5 as amended by that ruling.
//
// WHY IT EXISTS. Under D5 the chain is
//
//     Listener(streamKey, field) -> node subtree -> pipeline stages -> terminal
//
// and each arrow is a real edge. A FAN-IN breaks that arrow. Its data comes
// from its own DECLARED `inputs`; a value arriving from upstream is a CLOCK for
// those inputs and is never forwarded on — that is the §5 Q1 ruling, and
// `CompositeRoot::onValue` below is where it lives: it forwards to
// `clockTargets_` and TO NOTHING ELSE. So when a fan-in sits in the `pipeline`
// array with anything built upstream of it, that upstream's output is consumed
// and dropped. Where the fan-in's declared inputs are all `Listener`s —
// 45 of the corpus's 52 fan-in requests are that shape — `clockTargets_` is
// EMPTY and the upstream's output is discarded in total silence: no log, no
// metric, no error, and a chart that is missing a value nobody can explain.
//
// WHAT IT COSTS, MEASURED — AND IT IS NOT NOTHING. SPEC §5 Q7 says "Nothing to
// forum ... it cannot emit a fan-in pipeline stage at all". That is true of
// `Aggregate` (forum's translator always turns the edge-fanned join into the
// Request's Node) and FALSE of `Pack` and `Let`, which forum treats as
// config-driven LINEAR stages (`forum/internal/pipelinetranslate/
// config_mapping.go` `case "pack"` / `case "let"`) and `translateStage` will
// place at ANY index of the chain. Driving forum's real `Translate()` on three
// graphs produced, verbatim:
//
//   Listener->Pack->Filter->Field->Responder  (the ENC-672 demo headline)
//       -> pipeline:[Pack, Filter, Field]                      ACCEPTED here
//   Listener->Filter->Pack->Field->Responder
//       -> pipeline:[Filter, Pack, Field]                      REFUSED here
//   Listener->{2 accessors}->Aggregate->Pack->Field->Responder
//       -> node:Aggregate + pipeline:[Pack, Field]             REFUSED here
//
// So forum CAN author both refused shapes, and this refusal stops them
// building. Measured at the pre-change commit `b273278`, all three emit the
// IDENTICAL sequence — the Pack's own record, projected — because the upstream
// half really is thrown away: deleting the `Filter`/`Aggregate` changes
// nothing. That is the defect, not a cost of removing it. But the honest
// statement is "two shapes forum can emit stop building", not "nothing to
// forum", and the first ACCEPTED row above is why the accept half must not be
// widened by one condition: it is forum's flagship graph.
//
// WHY IT IS A BUILD-TIME REFUSAL AND NOT A DOCUMENTED BEHAVIOUR. **0 of the 272
// `tests/treebuilder/corpus_requests.json` entries have a fan-in in a pipeline
// stage** (all 52 `Aggregate`s are under `node`; the corpus contains no `Pack`
// and no `Let` — its only pipeline stage type at all is `Worker`, 104 uses). So
// no corpus count, no build smoke test and no value assertion goes red on the
// day someone first authors the shape. That is exactly D7/ENC-1293's argument,
// made by the same evidence, and it reaches the same verdict: convert a silent
// drop into a loud build error. A refusal can be lifted if a use appears; a
// silent discard that ships becomes behaviour someone depends on.
//
// THE ONE ACCEPTED PLACEMENT IS NOT AN EXCEPTION — IT IS §5 Q1. A fan-in as the
// FIRST element of `pipeline`/`stages` in a request with NO `node` key has the
// head `Listener` as its upstream and nothing else, because `midHead` starts at
// `terminal` either way: that is bit-for-bit the wiring the same fan-in gets
// under `node` with no pipeline, which §5 Q1 already ruled correct. Refusing it
// would refuse a shape semantically identical to the blessed one and would
// break requests that work today — `RecordTerminalTest`'s
// `Pack -> Field -> Responder` and `ClientSessionTest`'s
// `SubscribeAcceptsPackFieldResponder` are both written in exactly that form.
//
// DELIBERATELY NOT RECURSIVE into a stage's own sub-JSON — AND THAT IS A REAL,
// MEASURED HOLE, not a vacuous one. SPEC §5 Q7 says "today no builder reachable
// from a pipeline stage takes a sub-node that could hold a fan-in except a
// fan-in's own `inputs`, so the question is currently empty." **That is false.**
// `Chain` holds `stages`, `Tee` holds `outputs`, `Switch` holds `cases`, and
// `Chain`'s builder ends `return curDown;` — it hands back its inner builder's
// head verbatim, so a `Chain` wrapping a fan-in makes the STAGE HEAD literally
// a `CompositeRoot`. Measured on this branch: `node:Worker{last}` +
// `pipeline:[Chain{stages:[Aggregate{Listener ask, Listener bid}]}]` builds,
// and the node's output does not appear at the terminal — exactly the failure
// this check exists to abolish, one keyword away. Reproduced identically
// through `Tee`. `FanInWrappedInAChainIsNotRefused_KnownGap` pins both.
//
// It stays out of scope because ENC-1336 was scoped non-recursive and because
// inventing a rule for the nested shape would be inventing behaviour the SPEC
// has not ruled. Widen this only with a ruling that says what the nested shape
// should MEAN — but widen it from the fact above, not from Q7's premise.
//
namespace {

// The node types whose builders construct a `CompositeRoot` — the fan-ins.
// These are exactly the three `std::make_shared<CompositeRoot>(...)` sites in
// the registrations at the bottom of this file: "Aggregate", "Pack", "Let".
//
// ADDING A FAN-IN? Add its type name here in the SAME commit that registers it.
// `FanInAsAPipelineStageUnderANodeIsRefused_AllThreeTypes` in
// tests/treebuilder/ComposedChainTest.cpp gates all three names that exist
// today. It CANNOT gate the absence of a fourth, and neither can the corpus
// (see the 0-of-272 note above) — a fan-in registered without being listed here
// goes back to discarding its upstream in silence, and nothing goes red. That
// unclosable gap is why this paragraph is a warning rather than a cross-ref.
bool isFanInType(const std::string& type) {
  return type == "Aggregate" || type == "Pack" || type == "Let"
      || type == "Worker";  // MUTANT: over-broad
}

std::string fanInPipelineStageMessage(const std::string& type,
                                      const char*        key,
                                      std::size_t        index,
                                      bool               hasNode) {
  const std::string where =
    std::string(key) + "[" + std::to_string(index) + "]";
  // `index - 1` is only reached with index >= 1: the caller returns early for
  // the one accepted placement (!hasNode && index == 0), so !hasNode implies
  // index != 0. Guarded anyway rather than relying on that at a distance — a
  // size_t underflow here would print `pipeline[18446744073709551615]`.
  const std::string because =
    (hasNode || index == 0)
      ? "this request also carries a 'node', whose subtree is built directly "
        "upstream of the first pipeline stage"
      : "it is not the first stage — " + std::string(key) + "[" +
        std::to_string(index - 1) + "] precedes it and would be built directly"
        " upstream of it";

  return "buildForRequest: node type '" + type + "' is a FAN-IN and it appears"
         " as " + where + " of this request, with something upstream of it in"
         " the composed chain: " + because + ". That is rejected at build time."
         " A fan-in takes its data from its own declared inputs; a value"
         " arriving from upstream is delivered as a CLOCK for those inputs,"
         " never as a join member (SPEC section 5 Q1), so this stage is a break"
         " in the chain and not a link in it. For 'Aggregate' and 'Pack' the"
         " upstream's output never reaches this stage's downstream at all, and"
         " where the declared inputs each carry their own Listener the"
         " forwarding set is EMPTY and that output is discarded entirely, with"
         " no log, no metric and no error — a chart missing a value with no"
         " diagnostic in either repo. ('Let' is the one shape where a clock"
         " reaching a body that carries no Listener is forwarded on; it is"
         " refused here too because this rule is structural rather than a"
         " per-shape audit.) THE FIX: move the"
         " fan-in to 'node' position. The composed chain is"
         " Listener(streamKey, field) -> node subtree -> pipeline stages ->"
         " terminal, so a fan-in under 'node' is clocked by the head Listener"
         " and its output flows THROUGH every pipeline stage instead of being"
         " thrown away; anything you had upstream of it belongs either inside"
         " one of the fan-in's own inputs, where it is a real join member, or"
         " downstream of it as a later pipeline stage. The ONE accepted"
         " placement in the pipeline is as " + std::string(key) + "[0] of a"
         " request with NO 'node' key — there the fan-in's upstream is the head"
         " Listener itself, which is the same wiring it gets under 'node'. See"
         " specs/2026-09-20-gma-join-correctness/SPEC.md section 5 Q7 and D5"
         " (ENC-1336).";
}

} // namespace

//
// ---------- CompositeRoot: fan-out root for many inputs ----------
//
// ENC-1290 / SPEC specs/2026-09-20-gma-join-correctness D5, section 5 Q1
// (ruled 2026-09-20 by ENC-1317, then corrected by that ticket's adversarial
// review — Corrections C4.0).
//
// THE OUTER `Listener(streamKey, field)` IS A CLOCK, NOT A DATA SOURCE.
// A request carrying `node` always builds a head Listener (`buildForRequest`
// throws without `streamKey`/`field`), and for a FAN-IN node that Listener is
// not one of the join's members — the members are the node's own declared
// `inputs`. So the head Listener's value must never enter the join's buffer;
// it exists to *clock* inputs that have no clock of their own.
//
// `clockTargets_` is that set, and `onValue` forwards to it AND TO NOTHING
// ELSE. It is filled by each fan-in builder from its DECLARED INPUT HEADS
// only, and only from those whose declared subtree contains no `Listener`
// (see `declaredInputIsSelfClocked`).
//
// `roots_` IS NOT THAT SET, AND MUST NEVER BE USED AS ONE. It is a lifecycle
// list: every fan-in builder appends the fan-in node ITSELF to it (`agg`,
// `pack`, the `Tee`s and the body head below), because Listeners hold only a
// weak_ptr to their downstream. Forwarding the clock to `roots_` — or to "the
// roots that are not Dispatcher-subscribed", which was the rule's first and
// retracted wording — pushes the clock's value straight into
// `Aggregate::onValue` -> `buf_[sv.symbol]` AS A JOIN MEMBER, manufacturing
// SPEC section 1.1 defect 2 inside the builder. `ClockIsNeverAJoinMember` in
// tests/treebuilder/ComposedChainTest.cpp is the gate on that.
//
// SCOPE. Only the three fan-in builders construct a CompositeRoot. A
// single-input transform node (Worker, AtomicAccessor, Interval, GroupSplit)
// is returned as its own head and is wired to the head Listener directly, so
// for it the outer Listener IS the data source — today and after D5. All 162
// `node`-only corpus requests are in that second category and none of them
// builds a CompositeRoot; nothing here touches them.
//
namespace {

class CompositeRoot final : public gma::INode {
public:
  CompositeRoot(std::vector<std::shared_ptr<gma::INode>> roots,
                std::vector<std::weak_ptr<gma::INode>>   clockTargets)
    : roots_(std::move(roots)), clockTargets_(std::move(clockTargets)) {}

  void onValue(const gma::StreamValue& sv) override {
    // The clock tick. Empty for every fan-in whose inputs are all Listeners
    // (45 of the 52 `node`+`pipeline` corpus entries), which is exactly the
    // pre-ENC-1290 no-op.
    if (stopping_.load(std::memory_order_acquire)) return;
    for (const auto& w : clockTargets_) {
      // `clockTargets_` is immutable after construction, so no lock is needed
      // here; the strong refs live in `roots_`, and locking the weak_ptr keeps
      // the target alive for the duration of the call even if shutdown() is
      // concurrently clearing `roots_`.
      if (auto t = w.lock()) t->onValue(sv);
    }
  }

  void shutdown() noexcept override {
    stopping_.store(true, std::memory_order_release);
    for (auto& r : roots_) {
      if (r) r->shutdown();
    }
    roots_.clear();
  }

private:
  std::vector<std::shared_ptr<gma::INode>>       roots_;         // lifecycle only
  const std::vector<std::weak_ptr<gma::INode>>   clockTargets_;  // the clock's fan-out
  std::atomic<bool>                              stopping_{false};
};

// Does this DECLARED input subtree carry its own clock?
//
// A `Listener` anywhere inside it means the Dispatcher drives it, so the outer
// Listener must not drive it a second time. Everything else — `AtomicAccessor`
// above all — is a pull node with no subscription: without the clock its
// branch of the join never fires at all, which is what makes the 7 pull-only
// corpus entries (ids 111-115, 192, 200) a DEAD join today rather than the
// "two live chains" SPEC section 1.1 defect 1 describes (corrected there by
// C2.1).
//
// The test is structural and build-time, over the request JSON, and is never a
// property of the authored `streamKey`/`field` — which is why all 52 affected
// corpus entries are RE-WIRED and none is re-authored.
//
// THREE THINGS SELF-CLOCK, and the list is exhaustive by construction — a node
// is self-clocked iff something other than its upstream calls its `onValue`:
//   * `Listener`      — the Dispatcher pushes to it.
//   * `Interval` /
//     `BucketTime`    — a timer thread drives their child directly
//                       (`timerLoop`). Their own `onValue` is an explicit
//                       no-op ("source node: no upstream input"), so before
//                       ENC-1290 named them here the clock was forwarded to
//                       them and was correct ONLY by accident, via that no-op.
//                       Saying it out loud costs nothing and stops the next
//                       edit to `Interval::onValue` from silently double-firing
//                       every timer-headed join input.
//
// `Ref` IS NOT ONE OF THEM, and an earlier draft of this function wrongly said
// it was. A `Ref`'s own head is a `RefStub` no-op, so clocking it is harmless —
// but the test runs over the WHOLE declared subtree, so `Ref -> true` poisoned
// every enclosing node: a `Let` whose bindings are pull-only and whose body
// merely mentions a `Ref` (i.e. every non-degenerate `Let`) was declared
// self-clocked, and its join stayed dead — the exact failure this rule exists
// to end for the 7 pull-only corpus entries. Caught by adversarial review; the
// `Let` cases in tests/treebuilder/ComposedChainTest.cpp are the gate, and no
// test covered the clause before.
//
// CONSERVATIVE DIRECTION. Returning `true` (do not clock) reproduces the
// pre-ENC-1290 behaviour exactly, so it is the safe answer when we cannot
// tell — hence depth overflow -> `true`. Returning a wrong `false` is the
// harmful direction: it injects the clock where something else already drives
// the input.
bool declaredInputIsSelfClocked(const rapidjson::Value& spec, int depth = 0) {
  if (depth > kMaxShapeDepth) return true;
  if (spec.IsArray()) {
    for (const auto& v : spec.GetArray())
      if (declaredInputIsSelfClocked(v, depth + 1)) return true;
    return false;
  }
  if (!spec.IsObject()) return false;
  if (spec.HasMember("type") && spec["type"].IsString()) {
    const char* t = spec["type"].GetString();
    if (std::strcmp(t, "Listener")   == 0) return true;
    if (std::strcmp(t, "Interval")   == 0) return true;
    if (std::strcmp(t, "BucketTime") == 0) return true;
  }
  for (auto m = spec.MemberBegin(); m != spec.MemberEnd(); ++m)
    if (declaredInputIsSelfClocked(m->value, depth + 1)) return true;
  return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// SUB-BUILD UNWIND (ENC-1291)
//
// `buildForRequest`'s `Unwind` guard (section 1.5, ENC-1290) only ever receives
// the node `buildOne` RETURNS. Anything constructed INSIDE a sub-build that
// then throws is invisible to it — and a fan-in builder is exactly that: it
// loops over its declared `inputs`/`fields`/`bindings` calling `buildOne`, so a
// throw on input N strands every `Listener` inputs 0..N-1 already registered
// with the Dispatcher. Those are the unbounded, client-reachable, never-
// unregistered subscriptions section 1.5 is about: still walked on every
// `onTick`, delivering to a downstream port that died with the rejected fan-in,
// so nothing observes them and nothing can stop them.
//
// This was live before ENC-1291 (a bad node type inside `inputs` reaches it),
// and ENC-1291 ADDED a throw site in that loop — the arity check fires for a
// NESTED fan-in too — so it is fixed here rather than left. Found by an
// adversarial pass that measured 25 stranded subscriptions from 25 refused
// builds of a nested `Aggregate`, on the committed tree.
//
// Used by all three fan-in builders, because the shape and the hole are the
// same in each. Gated by `NestedFanInThrowLeavesNothingSubscribed`.
struct SubBuildUnwind {
  std::vector<std::shared_ptr<gma::INode>> built;
  bool                                     armed{true};

  void keep(const std::shared_ptr<gma::INode>& n) { if (n) built.push_back(n); }
  void disarm() noexcept { armed = false; }

  ~SubBuildUnwind() {
    if (!armed) return;
    // Reverse order: tear down upstream before the downstream it feeds.
    for (auto it = built.rbegin(); it != built.rend(); ++it)
      if (*it) (*it)->shutdown();
  }
};

// RefStub: the no-op head a `Ref` returns (ENC-647). A Ref has no local
// upstream — its binding's producer feeds the Ref's downstream directly (via a
// Tee) — so this stub never receives values. It exists only to satisfy the
// builder contract (every buildOne returns an INode) and is kept alive by
// whatever consumes the Ref position (e.g. an Aggregate's roots).
class RefStub final : public gma::INode {
public:
  void onValue(const gma::StreamValue&) override {}
  void shutdown() noexcept override {}
};

} // namespace

//
// ---------- Worker function library (math over ArgType) ----------
//
namespace {

using Span_t = gma::Span<const gma::ArgType>;

// Convert ArgType to double for numeric ops
double toDouble(const gma::ArgType& v) {
  return std::visit(
    [](auto&& x) -> double {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, bool>)      return x ? 1.0 : 0.0;
      else if constexpr (std::is_same_v<T, int>)  return static_cast<double>(x);
      else if constexpr (std::is_same_v<T, double>) return x;
      else                                        return 0.0; // non-numeric -> 0
    },
    v
  );
}

gma::Worker::Fn fnFromName(const rapidjson::Value& spec) {
  if (!spec.HasMember("fn") || !spec["fn"].IsString())
    throw std::runtime_error("Worker: missing 'fn'");

  const std::string fn = spec["fn"].GetString();
  auto& fmap = gma::FunctionMap::instance();

  // Parametric reducer? Extract every numeric spec member as a named
  // parameter (filtering out structural/well-known keys), bind into the
  // ParamFunc, and return.
  if (fmap.isParametric(fn)) {
    std::map<std::string, double> params;
    for (auto it = spec.MemberBegin(); it != spec.MemberEnd(); ++it) {
      const std::string key = it->name.GetString();
      if (key == "fn" || key == "type" || key == "child" ||
          key == "node" || key == "inputs" || key == "stages" ||
          key == "pipeline") continue;
      if (it->value.IsNumber()) params[key] = it->value.GetDouble();
    }
    auto pfn = fmap.getParamFunction(fn);
    return [pfn, params = std::move(params)](Span_t xs) -> gma::ArgType {
      std::vector<double> dv;
      dv.reserve(xs.size());
      for (const auto& v : xs) dv.push_back(toDouble(v));
      return gma::ArgType{pfn(dv, params)};
    };
  }

  // Plain reducer: resolve through FunctionMap. Adapts FunctionMap's
  // double(vector<double>) to Worker's ArgType(Span<const ArgType>).
  try {
    auto mapFn = fmap.getFunction(fn);
    return [mapFn](Span_t xs) -> gma::ArgType {
      std::vector<double> dv;
      dv.reserve(xs.size());
      for (const auto& v : xs) dv.push_back(toDouble(v));
      return gma::ArgType{mapFn(dv)};
    };
  } catch (...) {
    throw std::runtime_error("Worker: unknown fn '" + fn + "'");
  }
}

} // namespace

//
// ---------- Builder implementation ----------
//
namespace gma::tree {

std::shared_ptr<gma::INode> buildOne(const rapidjson::Value& spec,
                                     const std::string&      defaultStreamKey,
                                     const Deps&             deps,
                                     std::shared_ptr<gma::INode> downstream);

std::shared_ptr<gma::INode> buildOne(const rapidjson::Value&      spec,
                                     const std::string&           defaultStreamKey,
                                     const Deps&                 deps,
                                     std::shared_ptr<gma::INode> downstream) {
  const auto& v    = expectObj(spec, "node");
  const std::string type = expectType(v);

  if (const auto* builder = gma::engine::NodeTypeRegistry::find(type)) {
    return (*builder)(v, defaultStreamKey, deps, downstream);
  }
  throw std::runtime_error("TreeBuilder: unknown node type '" + type + "'");
}

//
// -------- Public API: buildNode / buildSimple / buildForRequest --------
//

std::shared_ptr<gma::INode> buildTree(const rapidjson::Value& rootSpec,
                                      const Deps&             deps) {
  return buildOne(rootSpec, /*defaultStreamKey=*/"", deps, /*downstream=*/nullptr);
}

std::shared_ptr<gma::INode> buildNode(const rapidjson::Value&      spec,
                                      const std::string&           defaultStreamKey,
                                      const Deps&                  deps,
                                      std::shared_ptr<gma::INode>  terminal) {
  return buildOne(spec, defaultStreamKey, deps, std::move(terminal));
}

std::shared_ptr<gma::INode> buildSimple(const std::string&      streamKey,
                                        const std::string&      field,
                                        int                     pollMs,
                                        const Deps&             deps,
                                        std::shared_ptr<gma::INode> terminal) {
  if (field.empty())
    throw std::runtime_error("buildSimple: field is empty");
  if (!deps.store)
    throw std::runtime_error("buildSimple: missing store");

  auto accessor =
    std::make_shared<gma::AtomicAccessor>(streamKey, field, deps.store, terminal);

  if (pollMs > 0) {
    gma::rt::ThreadPool* pool = deps.pool;
    if (!pool && gma::gThreadPool)
      pool = gma::gThreadPool.get();
    if (!pool)
      throw std::runtime_error("buildSimple: no thread pool available");
    auto interval = std::make_shared<gma::Interval>(
        std::chrono::milliseconds(pollMs), accessor, pool);
    interval->start();
    return interval;
  }
  return accessor;

}

// High-level entry: request JSON -> Listener head wired into optional pipeline -> terminal
BuiltChain buildForRequest(const rapidjson::Value&      requestJson,
                           const Deps&                  depsIn,
                           std::shared_ptr<gma::INode>  terminal) {
  const auto& rq = expectObj(requestJson, "request");

  if (!rq.HasMember("streamKey") || !rq["streamKey"].IsString())
    throw std::runtime_error("buildForRequest: missing 'streamKey'");
  if (!rq.HasMember("field") || !rq["field"].IsString())
    throw std::runtime_error("buildForRequest: missing 'field'");

  const std::string streamKey = rq["streamKey"].GetString();
  const std::string field     = rq["field"].GetString();

  if (streamKey.empty())
    throw std::runtime_error("buildForRequest: 'streamKey' must not be empty");
  if (field.empty())
    throw std::runtime_error("buildForRequest: 'field' must not be empty");

  if (!terminal)
    throw std::runtime_error("buildForRequest: terminal node cannot be null");

  // ENC-1005 / SPEC specs/2026-09-20-gma-join-correctness D3, D4 — ONE STRAND
  // PER REQUEST DAG, MINTED HERE WHEN THE CALLER DID NOT MINT ONE.
  //
  // `ClientSession::handleSubscribe` mints one per subscription, which is what
  // D3 names. This is the backstop, and it is deliberate rather than defensive:
  // `buildForRequest` IS the request DAG's constructor — the one place that
  // sees the whole request and nothing but the request — so minting here makes
  // ordered delivery a property of the DAG that no caller can forget. Every
  // Listener built below, head and join inputs alike, shares this one strand;
  // sub-builders copy `Deps` and carry it through, so a `Let` body or a fan-in
  // input never gets an ordering of its own.
  //
  // Not minted when there is no pool: with no executor there is nothing to
  // serialise, delivery is already inline and already in order.
  Deps depsWithStrand = depsIn;
  if (!depsWithStrand.strand && depsWithStrand.pool)
    depsWithStrand.strand = std::make_shared<gma::rt::Strand>(depsWithStrand.pool);
  const Deps& deps = depsWithStrand;   // everything below builds against this

  // ENC-1336 / SPEC §5 Q7, D5 as amended — A FAN-IN IS NOT A PIPELINE STAGE
  // when anything is built upstream of it. See the long comment on
  // `isFanInType` above for why this is a refusal rather than a documented
  // behaviour, and for the one placement that stays legal.
  //
  // Like D7's check below, this is STRUCTURAL and reads only the request JSON,
  // and it runs BEFORE a single node is constructed — the Listener / Interval /
  // BucketTime builders spawn threads and call start(), so a late reject leaks
  // live work for a request that is never served (SPEC §1.5).
  //
  // It runs BEFORE D7's Record check deliberately: a misplaced `Pack` trips
  // both, and WHERE the stage sits is the more specific diagnosis — moving it
  // is the fix, and D7 will still speak up afterwards if the terminal really
  // would receive a Record. It is also the permanent rule of the two; D7's
  // block is scheduled for deletion by ENC-1295 and nothing here moves with it.
  {
    const bool hasNode = rq.HasMember("node") && rq["node"].IsObject();
    for (const char* k : {"pipeline", "stages"}) {
      if (!rq.HasMember(k) || !rq[k].IsArray()) continue;
      const auto arr = rq[k].GetArray();
      for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
        const auto& stage = arr[i];
        if (!stage.IsObject() || !stage.HasMember("type") ||
            !stage["type"].IsString())
          continue;                    // malformed — buildOne throws its own error
        const std::string type = stage["type"].GetString();
        if (!isFanInType(type)) continue;
        // THE ACCEPTED CASE, and it is §5 Q1 rather than an exception: with no
        // `node`, `midHead` starts at `terminal`, so the head Listener is this
        // stage's only upstream — the same wiring the fan-in gets under `node`.
        if (!hasNode && i == 0) continue;
        throw std::runtime_error(
          fanInPipelineStageMessage(type, k, static_cast<std::size_t>(i), hasNode));
      }
      break;                           // mirrors the build loop: first key wins
    }
  }
  // ENC-1293 / SPEC specs/2026-09-20-gma-join-correctness D7 — TEMPORARY, and
  // lifted by ENC-1295 (embassy). See the long comment on `shapeInto` above for
  // why a Record reaching the terminal is a SILENT failure in two repos.
  //
  // This runs before anything is constructed: the Listener / Interval /
  // BucketTime builders spawn threads and call start(), so a reject after the
  // fact would leak live work for a request that is never going to be served.
  //
  // NARROWED BY ENC-1290 (SPEC D5, §5 Q1 "Consequence for D7 and ENC-1293").
  //
  // This used to analyse `node` and the pipeline as two INDEPENDENT feeders of
  // the terminal, which was correct while they were two live chains (SPEC §1.1
  // defect 1). They are now ONE chain — `node` subtree -> pipeline stages ->
  // terminal — so exactly one thing feeds the terminal: the LAST pipeline stage
  // when a pipeline is present, the `node` subtree's output when it is not.
  //
  // Threading the node's output shape INTO the pipeline (rather than starting
  // the pipeline from Opaque and OR-ing the two verdicts) is the whole point:
  // it is what lets `node:Pack` + `pipeline:[Field]` through. That is the
  // canonical shape D7 blesses one line above its own restriction, and the old
  // form rejected it. Nothing in the corpus goes red on the day that becomes
  // wrong — the 272 entries construct no Record at all — so the gate is
  // `NodePackPipelineFieldIsAcceptedAndEmits` in
  // tests/treebuilder/ComposedChainTest.cpp, not a corpus count.
  {
    std::string culprit;
    // The chain's own upstream is the head Listener, which emits a scalar.
    ValueShape intoTerminal = ValueShape::Opaque;

    if (rq.HasMember("node") && rq["node"].IsObject())
      intoTerminal = shapeInto(rq["node"], intoTerminal, nullptr, 0, &culprit);

    for (const char* k : {"pipeline", "stages"}) {
      if (!rq.HasMember(k) || !rq[k].IsArray()) continue;
      for (const auto& stage : rq[k].GetArray())
        intoTerminal = shapeInto(stage, intoTerminal, nullptr, 0, &culprit);
      break;                                // mirrors the build loop: first key wins
    }

    if (intoTerminal == ValueShape::Record)
      throw std::runtime_error(recordTerminalMessage(culprit));
  }

  // Collect every node so callers can keep them alive (all use weak_ptr downstream).
  std::vector<std::shared_ptr<gma::INode>> keepAlive;
  keepAlive.push_back(terminal);

  // ENC-1290 / SPEC D5 — `node` and `pipeline` COMPOSE INTO ONE CHAIN:
  //
  //     Listener(streamKey, field) -> node subtree -> pipeline stages -> terminal
  //
  // and nothing reaches the terminal except through the pipeline.
  //
  // This used to build the `node` subtree wired straight to `terminal` and then
  // restart from `terminal` for the pipeline, overwriting `midHead` — leaving
  // BOTH chains live on one Responder, so the client received two unrelated
  // streams interleaved on one request key and the pipeline never saw the
  // values it exists to reduce (SPEC §1.1 defect 1; 52 of the 272 checked-in
  // corpus requests carry both keys).
  //
  // ORDER MATTERS AND IS NOW REVERSED: the pipeline is built first, from the
  // terminal backwards, so that the `node` subtree can be built INTO its head.
  //
  // A PARTIAL BUILD MUST NOT SURVIVE A THROW. `Dispatcher::_listeners` holds a
  // `shared_ptr<INode>` per subscription and the ONLY thing that unregisters is
  // `Listener::shutdown()` (`src/nodes/Listener.cpp`), so every Listener built
  // before a later builder throws would stay subscribed for the life of the
  // process, with nothing left holding a handle to shut it down. `Interval` and
  // `BucketTime` likewise have a live thread by the time their builder returns.
  //
  // That leak is older than ENC-1290 — the previous order leaked on the mirror
  // input (a good `node` followed by a bad pipeline stage) — but reversing the
  // order moved the trigger onto the `node` subtree, which is the deeper and
  // far more failure-prone one (`unknown node type`, `Aggregate: empty
  // 'inputs'`, `Worker: unknown fn`, `Ref: unknown binding`, `Interval:
  // positive 'ms' required`). It is reachable from untrusted client JSON:
  // `ClientSession::handleSubscribe` catches the throw and replies
  // `{"type":"error","where":"build"}`, so a client can drive it in a loop.
  // Measured before this guard: 1000 rejected builds left 1000 subscriptions
  // live and took `onTick` from 0.02 ms to 932 ms for 50 ticks, growing
  // without bound.
  //
  // So everything constructed below is torn down on the way out unless the
  // build reaches the end. This fixes BOTH orders, not just the new one.
  struct Unwind {
    std::vector<std::shared_ptr<gma::INode>>* built;
    bool                                      armed{true};
    ~Unwind() {
      if (!armed || !built) return;
      // Reverse order: tear down upstream before the downstream it feeds.
      for (auto it = built->rbegin(); it != built->rend(); ++it)
        if (*it) (*it)->shutdown();
    }
  };
  // Deliberately NOT `keepAlive`, whose first element is the CALLER's terminal
  // — that is never ours to shut down. `Listener::Create` is the last thing
  // that can fail, and it reports failure instead of throwing, so the head
  // never needs unwinding: on its error path it was never constructed.
  std::vector<std::shared_ptr<gma::INode>> ours;
  Unwind unwind{&ours};

  std::shared_ptr<gma::INode> midHead = terminal;

  // The pipeline, built tail-first. `midHead` stays `terminal` when absent.
  const char* pipeKeys[] = {"pipeline", "stages"};
  for (const char* k : pipeKeys) {
    if (rq.HasMember(k) && rq[k].IsArray()) {
      auto curDown = terminal;
      const auto& arr = rq[k];
      for (size_t i = arr.Size(); i > 0; --i) {
        curDown = buildOne(arr[static_cast<rapidjson::SizeType>(i - 1)],
                           streamKey,
                           deps,
                           curDown);
        keepAlive.push_back(curDown);
        ours.push_back(curDown);
      }
      midHead = curDown;
      break;
    }
  }

  // The `node` subtree, upstream of whatever the pipeline left in `midHead`.
  if (rq.HasMember("node") && rq["node"].IsObject()) {
    midHead = buildOne(rq["node"], streamKey, deps, midHead);
    keepAlive.push_back(midHead);
    ours.push_back(midHead);
  }

  if (!deps.dispatcher || !deps.pool)
    throw std::runtime_error("buildForRequest: missing dispatcher/pool");

  using gma::nodes::Listener;
  auto headRes = Listener::Create(streamKey,
                                  field,
                                  midHead,
                                  deps.pool,
                                  deps.dispatcher,
                                  deps.strand);   // ENC-1005 / D3
  if (!headRes) {
    // Propagate the ENC-101 reject (and any future Listener::Create
    // pre-flight errors) up through ClientSession's
    // try { buildForRequest(...) } catch (std::exception&) {
    //   sendError("build", ex.what());
    // } chain (the catch wrapping the buildForRequest call in
    // ClientSession::handleSubscribe) — which produces a
    // {"type":"error","where":"build","message":...} WS response.
    // (The separate "validate" catch only wraps JsonValidator::validateTree
    // on the pipeline/stages/node sub-trees, not this build step.)
    throw std::runtime_error(headRes.error().message);
  }
  auto head = std::move(headRes.value());

  unwind.armed = false;      // the build succeeded; the caller owns it all
  BuiltChain out;
  out.head      = head;
  out.keepAlive = std::move(keepAlive);
  return out;
}

} // namespace gma::tree

// ---------- Builtin node-type registrations ----------
//
// Each entry below wraps the equivalent of the old buildOne if-branch in a
// lambda and publishes it to NodeTypeRegistry. Registration is idempotent on
// duplicates (registerNodeType returns false) so this function is safe to
// call repeatedly.
namespace gma {

void registerBuiltinNodeTypes() {
  using gma::engine::NodeTypeRegistry;
  using gma::engine::NodeBuilderFn;

  NodeTypeRegistry::registerNodeType("Listener",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!deps.dispatcher || !deps.pool)
        throw std::runtime_error("Listener: missing dispatcher/pool");

      const std::string streamKey = strOr(v, "streamKey", defaultStreamKey);
      const std::string field     = strOr(v, "field",  "");
      if (streamKey.empty())
        throw std::runtime_error("Listener: missing 'streamKey'");
      if (field.empty())
        throw std::runtime_error("Listener: missing 'field'");

      using gma::nodes::Listener;
      // ENC-1005 / SPEC D3: the same strand as every other Listener in this
      // request. This is the site that matters for a join — corpus 86's `ask`
      // and `bid` inputs are both built here, and sharing one strand is what
      // makes them arrive in the order the Dispatcher produced them.
      auto sp = std::make_shared<Listener>(streamKey, field, downstream,
                                           deps.pool, deps.dispatcher,
                                           deps.strand);
      sp->start();
      return sp;
    });

  NodeTypeRegistry::registerNodeType("Interval",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      rt::ThreadPool* pool = deps.pool;
      if (!pool && gThreadPool) pool = gThreadPool.get();
      if (!pool)
        throw std::runtime_error("Interval: no thread pool available");

      int ms = intOr(v, "ms", intOr(v, "periodMs", 0));
      if (ms <= 0)
        throw std::runtime_error("Interval: positive 'ms' required");
      static constexpr int MAX_INTERVAL_MS = 3600000;
      if (ms > MAX_INTERVAL_MS)
        throw std::runtime_error("Interval: 'ms' exceeds maximum (3600000)");

      auto child = downstream;
      if (v.HasMember("child"))
        child = tree::buildOne(v["child"], defaultStreamKey, deps, downstream);

      auto interval = std::make_shared<Interval>(
          std::chrono::milliseconds(ms), child, pool);
      interval->start();
      return interval;
    });

  // BucketTime emits ticks aligned to wall-clock period boundaries (e.g.
  // ms=60000 ticks at every minute boundary regardless of when the node
  // was constructed). Same JSON shape as Interval — pipelines that need
  // alignment swap "Interval" for "BucketTime" without other changes.
  NodeTypeRegistry::registerNodeType("BucketTime",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      rt::ThreadPool* pool = deps.pool;
      if (!pool && gThreadPool) pool = gThreadPool.get();
      if (!pool)
        throw std::runtime_error("BucketTime: no thread pool available");

      int ms = intOr(v, "ms", intOr(v, "periodMs", 0));
      if (ms <= 0)
        throw std::runtime_error("BucketTime: positive 'ms' required");
      static constexpr int MAX_BUCKET_MS = 3600000;
      if (ms > MAX_BUCKET_MS)
        throw std::runtime_error("BucketTime: 'ms' exceeds maximum (3600000)");

      auto child = downstream;
      if (v.HasMember("child"))
        child = tree::buildOne(v["child"], defaultStreamKey, deps, downstream);

      auto bucket = std::make_shared<BucketTime>(
          std::chrono::milliseconds(ms), child, pool);
      bucket->start();
      return bucket;
    });

  // TumblingWindow taps an upstream scalar stream, buffers per (streamKey)
  // until each wall-clock-aligned boundary, then emits one
  // StreamValue{symbol, vector<double>} downstream and clears. Pipeline-
  // stage shape (no "child" / no "input" key) — wired via the standard
  // pipeline-array reverse-iteration in buildForRequest; the OUTER caller
  // passes `downstream`.
  NodeTypeRegistry::registerNodeType("TumblingWindow",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      rt::ThreadPool* pool = deps.pool;
      if (!pool && gThreadPool) pool = gThreadPool.get();
      if (!pool)
        throw std::runtime_error("TumblingWindow: no thread pool available");

      int ms = intOr(v, "ms", intOr(v, "periodMs", 0));
      if (ms <= 0)
        throw std::runtime_error("TumblingWindow: positive 'periodMs' required");
      static constexpr int MAX_TUMBLING_MS = 3600000;
      if (ms > MAX_TUMBLING_MS)
        throw std::runtime_error("TumblingWindow: 'periodMs' exceeds maximum (3600000)");

      auto tw = std::make_shared<TumblingWindow>(
          std::chrono::milliseconds(ms), downstream, pool);
      tw->start();
      return tw;
    });

  // VectorReducer consumes one StreamValue{vector<double>} per upstream
  // emit (typically from TumblingWindow), applies `fn` from the shared
  // FunctionMap registry, and forwards a scalar downstream. Pipeline-
  // stage shape; reuses the same registry Worker resolves through, but
  // consumes FunctionMap's native double(vector<double>) signature
  // directly — no variant-typed adapter needed.
  NodeTypeRegistry::registerNodeType("VectorReducer",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& /*deps*/, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("fn") || !v["fn"].IsString())
        throw std::runtime_error("VectorReducer: missing 'fn'");
      const std::string fn = v["fn"].GetString();
      gma::Func reducer;
      try {
        reducer = gma::FunctionMap::instance().getFunction(fn);
      } catch (...) {
        throw std::runtime_error("VectorReducer: unknown fn '" + fn + "'");
      }
      return std::make_shared<VectorReducer>(std::move(reducer), downstream);
    });

  NodeTypeRegistry::registerNodeType("AtomicAccessor",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!deps.store)
        throw std::runtime_error("AtomicAccessor: missing store");

      const std::string streamKey = strOr(v, "streamKey", defaultStreamKey);
      const std::string field     = strOr(v, "field",  "");
      if (field.empty())
        throw std::runtime_error("AtomicAccessor: missing 'field'");

      return std::make_shared<AtomicAccessor>(streamKey, field, deps.store, downstream);
    });

  NodeTypeRegistry::registerNodeType("Worker",
    [](const rapidjson::Value& v, const std::string&,
       const tree::Deps&, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      auto fn = fnFromName(v);
      return std::make_shared<Worker>(std::move(fn), downstream);
    });

  NodeTypeRegistry::registerNodeType("Aggregate",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      // ENC-1291 / SPEC D2. EVERYTHING IS VALIDATED BEFORE ANYTHING IS BUILT.
      // `Listener`/`Interval`/`BucketTime` builders subscribe and spawn threads
      // as they are constructed, so a late throw costs real work (SPEC section
      // 1.5 measured what a partially-built, rejected subscription used to
      // leave behind). The RAII `Unwind` guard in buildForRequest now cleans
      // that up, but not building it in the first place is cheaper and is the
      // order D7's check already established.
      // `sizeOr` gates on `IsUint()`, so a present-but-wrongly-typed `arity`
      // (`2.0`, `"2"`, `-1`) silently takes the default and used to be reported
      // as "positive 'arity' required" — true, but it sends the author looking
      // for a missing field they did supply. Separate the two complaints
      // (ENC-1291; nothing covered this before
      // `NonIntegerArityIsRefusedForTheRightReason`).
      if (v.HasMember("arity") && !v["arity"].IsUint())
        throw std::runtime_error(
          "Aggregate: 'arity' must be a non-negative whole number");

      const std::size_t arity = sizeOr(v, "arity", 0);
      if (arity == 0)
        throw std::runtime_error("Aggregate: positive 'arity' required");

      if (!v.HasMember("inputs") || !v["inputs"].IsArray())
        throw std::runtime_error("Aggregate: 'inputs' must be an array");

      const auto& inputArr = v["inputs"];
      const std::size_t inputCount = inputArr.Size();
      if (inputCount == 0)
        throw std::runtime_error("Aggregate: empty 'inputs' array");

      // ENC-1291 / SPEC D2: `arity` used to be read by `sizeOr` and NEVER
      // compared to `inputs.size()`, so `{"arity":2,"inputs":[a,b,c,d,e]}`
      // built happily and then joined by value count. Now that an input's
      // identity IS its port index, the two numbers are the same number said
      // twice and a disagreement is unresolvable: there is no defensible
      // reading of "2 of these 5 inputs". Refuse it, naming BOTH counts so the
      // author can see which one they meant.
      //
      // Measured against the checked-in corpus at the commit that introduced
      // this: 0 of 272 `tests/treebuilder/corpus_requests.json` entries are
      // refused — all 52 `Aggregate` requests already agree (49 are 2/2, two
      // are 3/3, one is 4/4). So this costs the corpus nothing and forum
      // nothing (`internal/pipelinetranslate/translator.go:160` already
      // documents the contract as "arity must match"); it closes the door on a
      // hand-written request that silently means something the engine cannot
      // express.
      if (arity != inputCount)
        throw std::runtime_error(
          "Aggregate: 'arity' is " + std::to_string(arity) +
          " but 'inputs' declares " + std::to_string(inputCount) +
          " input(s) — they must be equal. A fan-in's inputs are addressed by "
          "position (SPEC specs/2026-09-20-gma-join-correctness D2), so "
          "'arity' is just the length of 'inputs' and cannot disagree with it.");

      auto agg = std::make_shared<Aggregate>(arity, downstream);

      // ENC-1291: a throw on input N must not strand inputs 0..N-1. See
      // SubBuildUnwind above.
      SubBuildUnwind unwind;
      unwind.keep(agg);

      std::vector<std::shared_ptr<INode>> roots;
      std::vector<std::weak_ptr<INode>>   clockTargets;   // ENC-1290 (D5/Q1)
      roots.reserve(inputCount + 1);
      std::size_t idx = 0;
      for (auto& it : inputArr.GetArray()) {
        // ENC-1291 / SPEC D2: each declared input terminates in its OWN
        // indexed port, so the join knows which input a value came from
        // without anything being added to StreamValue. `agg` owns the port;
        // the port holds only a weak_ptr back (include/gma/nodes/InputPort.hpp
        // documents why that direction is forced).
        auto port = std::make_shared<InputPort>(std::weak_ptr<IFanIn>(agg), idx++);
        agg->addPort(port);
        auto inHead = tree::buildOne(it, defaultStreamKey, deps, port);
        // A declared input with no Listener anywhere in it (AtomicAccessor and
        // friends) has no clock of its own; the request's head Listener is it.
        // An input that carries a Listener is already Dispatcher-driven and
        // must NOT be driven a second time.
        if (!declaredInputIsSelfClocked(it)) clockTargets.emplace_back(inHead);
        unwind.keep(inHead);
        roots.push_back(std::move(inHead));
      }

      // Keep Aggregate alive alongside input heads — Listeners hold only a
      // weak_ptr to their downstream, so without this the Aggregate would be
      // destroyed when the local shared_ptr goes out of scope.
      // NOTE (ENC-1290): this makes `roots` a LIFECYCLE list, not a list of
      // input heads. It is deliberately not the clock's fan-out set — see the
      // CompositeRoot comment.
      roots.push_back(agg);

      unwind.disarm();
      return std::make_shared<CompositeRoot>(std::move(roots),
                                             std::move(clockTargets));
    });

  // Canonical name "GroupSplit"; "SymbolSplit" registered as a legacy alias
  // for back-compat with pre-rename request payloads (Q5 of the engine /
  // connector split decision matrix). Drop the alias when the deprecation
  // window closes.
  auto groupSplitBuilder =
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("child"))
        throw std::runtime_error("GroupSplit: missing 'child'");

      // Deep-copy so the factory lambda owns the JSON independently of the
      // caller's stack-local Document.
      auto childDoc = std::make_shared<rapidjson::Document>();
      childDoc->CopyFrom(v["child"], childDoc->GetAllocator());

      GroupSplit::Factory f =
        [childDoc, defaultStreamKey, deps, downstream](const std::string& sym) {
          return tree::buildOne(*childDoc,
                                sym.empty() ? defaultStreamKey : sym,
                                deps, downstream);
        };
      return std::make_shared<GroupSplit>(std::move(f));
    };
  NodeTypeRegistry::registerNodeType("GroupSplit", groupSplitBuilder);
  NodeTypeRegistry::registerNodeType("SymbolSplit", groupSplitBuilder);

  NodeTypeRegistry::registerNodeType("Chain",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("stages") || !v["stages"].IsArray())
        throw std::runtime_error("Chain: 'stages' must be an array");

      const auto& stages = v["stages"];
      if (stages.Size() == 0)
        throw std::runtime_error("Chain: 'stages' must not be empty");

      auto curDown = downstream;
      for (size_t i = stages.Size(); i > 0; --i) {
        curDown = tree::buildOne(stages[static_cast<rapidjson::SizeType>(i - 1)],
                                 defaultStreamKey, deps, curDown);
      }
      return curDown;
    });

  // Tee fans each incoming value out to every output subtree, unchanged. Each
  // entry of "outputs" is built as an independent subtree terminating in the
  // shared `downstream`, so a Tee taps one upstream value into N parallel
  // branches. The data-plane multi-downstream primitive underpinning
  // let-bindings (ENC-647) and Switch (ENC-650).
  NodeTypeRegistry::registerNodeType("Tee",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("outputs") || !v["outputs"].IsArray())
        throw std::runtime_error("Tee: 'outputs' must be an array");

      const auto& arr = v["outputs"];
      if (arr.Size() == 0)
        throw std::runtime_error("Tee: 'outputs' must not be empty");

      std::vector<std::shared_ptr<INode>> outs;
      outs.reserve(arr.Size());
      for (auto& it : arr.GetArray())
        outs.push_back(tree::buildOne(it, defaultStreamKey, deps, downstream));

      return std::make_shared<Tee>(std::move(outs));
    });

  // Pack assembles N named input subtrees into a keyed Record per symbol
  // (combineLatest). Shape: {"type":"Pack","fields":{"o":<sub>,"h":<sub>,...}}.
  // Each field's input is built terminating in a per-field InputPort; the Pack
  // owns the ports and emits the Record into the shared `downstream`. Mirrors
  // Aggregate's fan-in ownership (CompositeRoot holds the input heads + Pack).
  NodeTypeRegistry::registerNodeType("Pack",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("fields") || !v["fields"].IsObject())
        throw std::runtime_error("Pack: 'fields' must be an object");

      const auto& fobj = v["fields"];
      if (fobj.MemberCount() == 0)
        throw std::runtime_error("Pack: 'fields' must not be empty");

      std::vector<std::string> names;
      names.reserve(fobj.MemberCount());
      for (auto it = fobj.MemberBegin(); it != fobj.MemberEnd(); ++it)
        names.push_back(it->name.GetString());

      auto pack = std::make_shared<Pack>(names, downstream);

      SubBuildUnwind unwind;          // ENC-1291, same hole as Aggregate's
      unwind.keep(pack);

      std::vector<std::shared_ptr<INode>> roots;
      std::vector<std::weak_ptr<INode>>   clockTargets;   // ENC-1290 (D5/Q1)
      roots.reserve(fobj.MemberCount() + 1);
      std::size_t idx = 0;
      for (auto it = fobj.MemberBegin(); it != fobj.MemberEnd(); ++it, ++idx) {
        auto port = std::make_shared<InputPort>(std::weak_ptr<IFanIn>(pack), idx);
        pack->addPort(port);
        auto inHead = tree::buildOne(it->value, defaultStreamKey, deps, port);
        if (!declaredInputIsSelfClocked(it->value)) clockTargets.emplace_back(inHead);
        unwind.keep(inHead);
        roots.push_back(std::move(inHead));
      }
      roots.push_back(pack);   // lifecycle, NOT a clock target (ENC-1290)

      unwind.disarm();
      return std::make_shared<CompositeRoot>(std::move(roots),
                                             std::move(clockTargets));
    });

  // Field extracts one named field from a Record flowing through. Pipeline
  // stage: {"type":"Field","name":"o"}. The inverse of Pack.
  NodeTypeRegistry::registerNodeType("Field",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& /*deps*/, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      const std::string name = strOr(v, "name", "");
      if (name.empty())
        throw std::runtime_error("Field: missing 'name'");
      return std::make_shared<Field>(name, downstream);
    });

  // Expr evaluates a compiled expression over each incoming value. Shape:
  // {"type":"Expr","expr":<expression-tree>}. A Record input exposes its fields
  // as named refs; a scalar is exposed as ref "value". The expression is
  // compiled once here (compile() throws on malformed input -> surfaces as a
  // build error). Compose Pack -> Expr to read several named inputs.
  NodeTypeRegistry::registerNodeType("Expr",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& /*deps*/, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("expr"))
        throw std::runtime_error("Expr: missing 'expr'");
      auto fn = gma::expr::compile(v["expr"]);
      return std::make_shared<ExprNode>(std::move(fn), downstream);
    });

  // Filter forwards each value unchanged iff a predicate expression over it is
  // truthy. Shape: {"type":"Filter","when":<expression-tree>}. The predicate
  // is compiled once (compile() throws on malformed -> build error).
  NodeTypeRegistry::registerNodeType("Filter",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& /*deps*/, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("when"))
        throw std::runtime_error("Filter: missing 'when'");
      auto pred = gma::expr::compile(v["when"]);
      return std::make_shared<Filter>(std::move(pred), downstream);
    });

  // Switch routes each value to one of N case branches by a selector expression
  // (rounded to an index); out-of-range routes to the optional default branch
  // or drops. Shape: {"type":"Switch","select":<expr>,"cases":[<sub>,...],
  // "default":<sub>?}. Each branch is built terminating in the shared
  // `downstream`. The selector is compiled once (throws on malformed).
  NodeTypeRegistry::registerNodeType("Switch",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("select"))
        throw std::runtime_error("Switch: missing 'select'");
      if (!v.HasMember("cases") || !v["cases"].IsArray())
        throw std::runtime_error("Switch: 'cases' must be an array");
      const auto& arr = v["cases"];
      if (arr.Size() == 0)
        throw std::runtime_error("Switch: 'cases' must not be empty");

      auto sel = gma::expr::compile(v["select"]);

      std::vector<std::shared_ptr<INode>> cases;
      cases.reserve(arr.Size());
      for (auto& it : arr.GetArray())
        cases.push_back(tree::buildOne(it, defaultStreamKey, deps, downstream));

      std::shared_ptr<INode> def;
      if (v.HasMember("default"))
        def = tree::buildOne(v["default"], defaultStreamKey, deps, downstream);

      return std::make_shared<Switch>(std::move(sel), std::move(cases), std::move(def));
    });

  // Ref taps a named let-binding (ENC-647). It registers its downstream as a
  // consumer of the binding in the active scope and returns a no-op stub — the
  // binding's producer (built by the enclosing Let) fans its values into this
  // downstream via a Tee. Errors if used outside a Let or naming an unknown
  // binding.
  NodeTypeRegistry::registerNodeType("Ref",
    [](const rapidjson::Value& v, const std::string& /*defaultStreamKey*/,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      const std::string name = strOr(v, "name", "");
      if (name.empty())
        throw std::runtime_error("Ref: missing 'name'");
      if (!downstream)
        throw std::runtime_error("Ref: '" + name + "' has no downstream consumer");

      tree::BindingScope* s = deps.bindingScope;
      while (s && !(s->bindings && s->bindings->HasMember(name.c_str())))
        s = s->parent;
      if (!s)
        throw std::runtime_error("Ref: unknown binding '" + name + "'");

      s->consumers[name].push_back(downstream);
      return std::make_shared<RefStub>();
    });

  // Let defines named bindings reusable by Ref across its body, turning the
  // tree into a reuse DAG. Shape: {"type":"Let","bindings":{name:<producer>},
  // "body":<subtree-with-Refs>}. Two passes: (1) build the body under a fresh
  // scope so Refs register their consumers; (2) for each referenced binding,
  // build its producer ONCE feeding a Tee over its consumers (or the lone
  // consumer directly). Unreferenced bindings are not built. Producers see the
  // OUTER scope only, so sibling bindings can't reference each other — no
  // cycles, preserving the bounded/total invariant.
  NodeTypeRegistry::registerNodeType("Let",
    [](const rapidjson::Value& v, const std::string& defaultStreamKey,
       const tree::Deps& deps, std::shared_ptr<INode> downstream)
        -> std::shared_ptr<INode> {
      if (!v.HasMember("bindings") || !v["bindings"].IsObject())
        throw std::runtime_error("Let: 'bindings' must be an object");
      if (!v.HasMember("body") || !v["body"].IsObject())
        throw std::runtime_error("Let: 'body' must be an object");

      const auto& bindings = v["bindings"];

      // Pass 1: build the body under a fresh scope; Refs register consumers.
      tree::BindingScope scope;
      scope.bindings = &bindings;
      scope.parent   = deps.bindingScope;
      tree::Deps bodyDeps = deps;
      bodyDeps.bindingScope = &scope;
      SubBuildUnwind unwind;          // ENC-1291, same hole as Aggregate's
      auto bodyHead = tree::buildOne(v["body"], defaultStreamKey, bodyDeps, downstream);
      unwind.keep(bodyHead);

      // Pass 2: build each referenced binding's producer once -> Tee/consumer.
      std::vector<std::shared_ptr<INode>> roots;
      std::vector<std::weak_ptr<INode>>   clockTargets;   // ENC-1290 (D5/Q1)
      for (auto& [name, consumers] : scope.consumers) {
        if (consumers.empty()) continue;

        std::shared_ptr<INode> sink;
        if (consumers.size() == 1) {
          sink = consumers.front();
        } else {
          auto tee = std::make_shared<Tee>(consumers);
          roots.push_back(tee);  // own the Tee (a Listener producer holds it weakly)
          sink = tee;            // lifecycle, NOT a clock target (ENC-1290)
        }

        auto prodHead =
          tree::buildOne(bindings[name.c_str()], defaultStreamKey, deps, sink);
        if (!declaredInputIsSelfClocked(bindings[name.c_str()]))
          clockTargets.emplace_back(prodHead);
        unwind.keep(prodHead);
        roots.push_back(prodHead);
      }

      // The body is a declared input head too — it is the branch that reaches
      // `downstream`, so a body with no Listener of its own is exactly what the
      // head Listener is there to drive.
      if (!declaredInputIsSelfClocked(v["body"])) clockTargets.emplace_back(bodyHead);
      roots.push_back(bodyHead);

      unwind.disarm();
      return std::make_shared<CompositeRoot>(std::move(roots),
                                             std::move(clockTargets));
    });
}

} // namespace gma
