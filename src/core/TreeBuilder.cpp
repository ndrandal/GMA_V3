#include "gma/TreeBuilder.hpp"
#include "gma/NodeRegistry.hpp"

#include <functional>
#include <algorithm>
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
// ---------- CompositeRoot: fan-out root for many inputs ----------
//
namespace {

class CompositeRoot final : public gma::INode {
public:
  explicit CompositeRoot(std::vector<std::shared_ptr<gma::INode>> roots)
    : roots_(std::move(roots)) {}

  void onValue(const gma::StreamValue&) override {
    // No-op. CompositeRoot is a lifecycle wrapper for multiple Listener
    // source nodes. Listeners receive values from Dispatcher, not
    // from upstream pipeline wiring.
  }

  void shutdown() noexcept override {
    for (auto& r : roots_) {
      if (r) r->shutdown();
    }
    roots_.clear();
  }

private:
  std::vector<std::shared_ptr<gma::INode>> roots_;
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
                           const Deps&                  deps,
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

  // Collect every node so callers can keep them alive (all use weak_ptr downstream).
  std::vector<std::shared_ptr<gma::INode>> keepAlive;
  keepAlive.push_back(terminal);

  // Optional mid-pipeline, ultimately forwarding into terminal
  std::shared_ptr<gma::INode> midHead = terminal;

  // Single node under "node"
  if (rq.HasMember("node") && rq["node"].IsObject()) {
    midHead = buildOne(rq["node"], streamKey, deps, terminal);
    keepAlive.push_back(midHead);
  }

  // Or an array pipeline under "pipeline" or "stages"
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
      }
      midHead = curDown;
      break;
    }
  }

  if (!deps.dispatcher || !deps.pool)
    throw std::runtime_error("buildForRequest: missing dispatcher/pool");

  using gma::nodes::Listener;
  auto headRes = Listener::Create(streamKey,
                                  field,
                                  midHead,
                                  deps.pool,
                                  deps.dispatcher);
  if (!headRes) {
    // Propagate the ENC-101 reject (and any future Listener::Create
    // pre-flight errors) up through ClientSession's
    // try { TreeBuilder } catch (std::exception&) {
    //   sendError("validate", ex.what());
    // } chain at src/server/ClientSession.cpp:456 — which produces a
    // {"type":"error","where":"validate","message":...} WS response.
    throw std::runtime_error(headRes.error().message);
  }
  auto head = std::move(headRes.value());

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
      auto sp = std::make_shared<Listener>(streamKey, field, downstream,
                                           deps.pool, deps.dispatcher);
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
      roots.reserve(inputArr.Size() + 1);
      for (auto& it : inputArr.GetArray())
        roots.push_back(tree::buildOne(it, defaultStreamKey, deps, agg));
      if (roots.empty())
        throw std::runtime_error("Aggregate: empty 'inputs' array");

      // Keep Aggregate alive alongside input heads — Listeners hold only a
      // weak_ptr to their downstream, so without this the Aggregate would be
      // destroyed when the local shared_ptr goes out of scope.
      roots.push_back(agg);

      if (roots.size() == 1) return roots.front();
      return std::make_shared<CompositeRoot>(std::move(roots));
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
}

} // namespace gma
