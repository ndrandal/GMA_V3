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
      std::size_t arity = sizeOr(v, "arity", 0);
      if (arity == 0)
        throw std::runtime_error("Aggregate: positive 'arity' required");

      auto agg = std::make_shared<Aggregate>(arity, downstream);

      if (!v.HasMember("inputs") || !v["inputs"].IsArray())
        throw std::runtime_error("Aggregate: 'inputs' must be an array");

      const auto& inputArr = v["inputs"];
      std::vector<std::shared_ptr<INode>> roots;
      std::vector<std::weak_ptr<INode>>   clockTargets;   // ENC-1290 (D5/Q1)
      roots.reserve(inputArr.Size() + 1);
      for (auto& it : inputArr.GetArray()) {
        auto inHead = tree::buildOne(it, defaultStreamKey, deps, agg);
        // A declared input with no Listener anywhere in it (AtomicAccessor and
        // friends) has no clock of its own; the request's head Listener is it.
        // An input that carries a Listener is already Dispatcher-driven and
        // must NOT be driven a second time.
        if (!declaredInputIsSelfClocked(it)) clockTargets.emplace_back(inHead);
        roots.push_back(std::move(inHead));
      }
      if (roots.empty())
        throw std::runtime_error("Aggregate: empty 'inputs' array");

      // Keep Aggregate alive alongside input heads — Listeners hold only a
      // weak_ptr to their downstream, so without this the Aggregate would be
      // destroyed when the local shared_ptr goes out of scope.
      // NOTE (ENC-1290): this makes `roots` a LIFECYCLE list, not a list of
      // input heads. It is deliberately not the clock's fan-out set — see the
      // CompositeRoot comment.
      roots.push_back(agg);

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
  // Each field's input is built terminating in a per-field PackPort; the Pack
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

      std::vector<std::shared_ptr<INode>> roots;
      std::vector<std::weak_ptr<INode>>   clockTargets;   // ENC-1290 (D5/Q1)
      roots.reserve(fobj.MemberCount() + 1);
      std::size_t idx = 0;
      for (auto it = fobj.MemberBegin(); it != fobj.MemberEnd(); ++it, ++idx) {
        auto port = std::make_shared<PackPort>(std::weak_ptr<Pack>(pack), idx);
        pack->addPort(port);
        auto inHead = tree::buildOne(it->value, defaultStreamKey, deps, port);
        if (!declaredInputIsSelfClocked(it->value)) clockTargets.emplace_back(inHead);
        roots.push_back(std::move(inHead));
      }
      roots.push_back(pack);   // lifecycle, NOT a clock target (ENC-1290)

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
      auto bodyHead = tree::buildOne(v["body"], defaultStreamKey, bodyDeps, downstream);

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
        roots.push_back(prodHead);
      }

      // The body is a declared input head too — it is the branch that reaches
      // `downstream`, so a body with no Listener of its own is exactly what the
      // head Listener is there to drive.
      if (!declaredInputIsSelfClocked(v["body"])) clockTargets.emplace_back(bodyHead);
      roots.push_back(bodyHead);
      return std::make_shared<CompositeRoot>(std::move(roots),
                                             std::move(clockTargets));
    });
}

} // namespace gma
