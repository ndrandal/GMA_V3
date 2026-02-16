#include "gma/TreeBuilder.hpp"

#include <functional>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

// Node types
#include "gma/nodes/Listener.hpp"
#include "gma/nodes/Aggregate.hpp"
#include "gma/nodes/SymbolSplit.hpp"
#include "gma/nodes/Worker.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/Interval.hpp"

// Runtime deps
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/MarketDispatcher.hpp"


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

  void onValue(const gma::SymbolValue&) override {
    // Composite roots are sources; they don't receive upstream values
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

  // mean / avg
  if (fn == "mean" || fn == "avg") {
    return [](Span_t xs) -> gma::ArgType {
      if (xs.size() == 0) return gma::ArgType{0.0};
      double s = 0.0;
      for (const auto& v : xs) s += toDouble(v);
      return gma::ArgType{s / xs.size()};
    };
  }

  // sum
  if (fn == "sum") {
    return [](Span_t xs) -> gma::ArgType {
      double s = 0.0;
      for (const auto& v : xs) s += toDouble(v);
      return gma::ArgType{s};
    };
  }

  // max
  if (fn == "max") {
    return [](Span_t xs) -> gma::ArgType {
      if (xs.size() == 0) return gma::ArgType{0.0};
      double m = -std::numeric_limits<double>::infinity();
      for (const auto& v : xs) m = std::max(m, toDouble(v));
      return gma::ArgType{m};
    };
  }

  // min
  if (fn == "min") {
    return [](Span_t xs) -> gma::ArgType {
      if (xs.size() == 0) return gma::ArgType{0.0};
      double m = std::numeric_limits<double>::infinity();
      for (const auto& v : xs) m = std::min(m, toDouble(v));
      return gma::ArgType{m};
    };
  }

  // spread = max - min
  if (fn == "spread") {
    return [](Span_t xs) -> gma::ArgType {
      if (xs.size() < 2) return gma::ArgType{0.0};
      double lo = std::numeric_limits<double>::infinity();
      double hi = -std::numeric_limits<double>::infinity();
      for (const auto& v : xs) {
        double dv = toDouble(v);
        lo = std::min(lo, dv);
        hi = std::max(hi, dv);
      }
      return gma::ArgType{hi - lo};
    };
  }

  // last
  if (fn == "last") {
    return [](Span_t xs) -> gma::ArgType {
      if (!xs.size()) return gma::ArgType{0.0};
      const auto& v = xs[xs.size() - 1];
      return gma::ArgType{toDouble(v)};
    };
  }


  // first
  if (fn == "first") {
    return [](Span_t xs) -> gma::ArgType {
      if (!xs.size()) return gma::ArgType{0.0};
      const auto& v = xs[0];
      return gma::ArgType{toDouble(v)};
    };
  }


  // diff = last - first
  if (fn == "diff") {
    return [](Span_t xs) -> gma::ArgType {
      if (xs.size() >= 2) {
        double a = toDouble(xs[0]);
        double b = toDouble(xs[xs.size() - 1]);
        return gma::ArgType{b - a};
      }
      return gma::ArgType{0.0};
    };
  }


  // scale: multiply last input by constant factor
  if (fn == "scale") {
    double factor =
      spec.HasMember("factor") && spec["factor"].IsNumber()
        ? spec["factor"].GetDouble()
        : 1.0;

    return [factor](Span_t xs) -> gma::ArgType {
      if (!xs.size()) return gma::ArgType{0.0};
      const auto& v = xs[xs.size() - 1];
      return gma::ArgType{toDouble(v) * factor};
    };
  }


  throw std::runtime_error("Worker: unknown fn '" + fn + "'");
}

} // namespace

//
// ---------- Builder implementation ----------
//
namespace gma::tree {

std::shared_ptr<gma::INode> buildOne(const rapidjson::Value& spec,
                                     const std::string&      defaultSymbol,
                                     const Deps&             deps,
                                     std::shared_ptr<gma::INode> downstream);

// Helper for Aggregate: build many inputs and wrap into CompositeRoot
static std::shared_ptr<gma::INode> buildManyAsRoot(const rapidjson::Value&      arr,
                                                   const std::string&           defaultSymbol,
                                                   const Deps&                 deps,
                                                   std::shared_ptr<gma::INode> downstream) {
  if (!arr.IsArray())
    throw std::runtime_error("TreeBuilder: 'inputs' must be an array");

  std::vector<std::shared_ptr<gma::INode>> roots;
  roots.reserve(arr.Size());

  for (auto& it : arr.GetArray()) {
    roots.push_back(buildOne(it, defaultSymbol, deps, downstream));
  }

  if (roots.empty())
    throw std::runtime_error("TreeBuilder: empty 'inputs' array");

  if (roots.size() == 1)
    return roots.front();

  return std::make_shared<CompositeRoot>(std::move(roots));
}

std::shared_ptr<gma::INode> buildOne(const rapidjson::Value&      spec,
                                     const std::string&           defaultSymbol,
                                     const Deps&                 deps,
                                     std::shared_ptr<gma::INode> downstream) {
  const auto& v    = expectObj(spec, "node");
  const std::string type = expectType(v);

  // --- Listener ---
  if (type == "Listener") {
    if (!deps.dispatcher || !deps.pool)
      throw std::runtime_error("Listener: missing dispatcher/pool");

    const std::string symbol = strOr(v, "symbol", defaultSymbol);
    const std::string field  = strOr(v, "field",  "");
    if (field.empty())
      throw std::runtime_error("Listener: missing 'field'");

    using gma::nodes::Listener;
    auto sp = std::make_shared<Listener>(symbol,
                                         field,
                                         downstream,
                                         deps.pool,
                                         deps.dispatcher);
    sp->start();
    return sp;
  }


  // --- Interval ---
  if (type == "Interval") {
    gma::rt::ThreadPool* pool = deps.pool;
    if (!pool && gma::gThreadPool)
      pool = gma::gThreadPool.get();
    if (!pool)
      throw std::runtime_error("Interval: no thread pool available");

    int ms = intOr(v, "ms", intOr(v, "periodMs", 0));
    if (ms <= 0)
      throw std::runtime_error("Interval: positive 'ms' required");

    const rapidjson::Value* childSpec =
      v.HasMember("child") ? &v["child"] : nullptr;

    auto child = downstream;
    if (childSpec)
      child = buildOne(*childSpec, defaultSymbol, deps, downstream);

    return std::make_shared<gma::Interval>(std::chrono::milliseconds(ms),
                                          child,
                                          pool);
  }


  // --- AtomicAccessor ---
  if (type == "AtomicAccessor") {
    if (!deps.store)
      throw std::runtime_error("AtomicAccessor: missing store");

    const std::string symbol = strOr(v, "symbol", defaultSymbol);
    const std::string field  = strOr(v, "field",  "");
    if (field.empty())
      throw std::runtime_error("AtomicAccessor: missing 'field'");

    return std::make_shared<gma::AtomicAccessor>(symbol, field,
                                                 deps.store,
                                                 downstream);
  }

  // --- Worker (batch math over inputs) ---
  if (type == "Worker") {
    auto fn = fnFromName(v);
    return std::make_shared<gma::Worker>(std::move(fn), downstream);
  }

  // --- Aggregate(N) ---
  if (type == "Aggregate") {
    std::size_t arity = sizeOr(v, "arity", 0);
    if (arity == 0)
      throw std::runtime_error("Aggregate: positive 'arity' required");

    auto agg = std::make_shared<gma::Aggregate>(arity, downstream);

    if (!v.HasMember("inputs"))
      throw std::runtime_error("Aggregate: missing 'inputs'");

    auto root = buildManyAsRoot(v["inputs"], defaultSymbol, deps, agg);
    return root;
  }

  // --- SymbolSplit ---
  if (type == "SymbolSplit") {
    if (!v.HasMember("child"))
      throw std::runtime_error("SymbolSplit: missing 'child'");

    const rapidjson::Value* childSpec = &v["child"];

    gma::SymbolSplit::Factory f =
      [childSpec, defaultSymbol, deps, downstream](const std::string& sym) {
        return buildOne(*childSpec,
                        sym.empty() ? defaultSymbol : sym,
                        deps,
                        downstream);
      };

    return std::make_shared<gma::SymbolSplit>(std::move(f));
  }

  // --- Chain (sequential composition) ---
  if (type == "Chain") {
    if (!v.HasMember("stages") || !v["stages"].IsArray())
      throw std::runtime_error("Chain: 'stages' must be an array");

    auto curDown = downstream;
    const auto& stages = v["stages"];
    for (int i = static_cast<int>(stages.Size()) - 1; i >= 0; --i) {
      curDown = buildOne(stages[static_cast<rapidjson::SizeType>(i)],
                         defaultSymbol,
                         deps,
                         curDown);
    }
    return curDown; // head of the chain
  }

  throw std::runtime_error("TreeBuilder: unknown node type '" + type + "'");
}

//
// -------- Public API: buildNode / buildSimple / buildForRequest --------
//

std::shared_ptr<gma::INode> buildTree(const rapidjson::Value& rootSpec,
                                      const Deps&             deps) {
  return buildOne(rootSpec, /*defaultSymbol=*/"", deps, /*downstream=*/nullptr);
}

std::shared_ptr<gma::INode> buildNode(const rapidjson::Value&      spec,
                                      const std::string&           defaultSymbol,
                                      const Deps&                  deps,
                                      std::shared_ptr<gma::INode>  terminal) {
  return buildOne(spec, defaultSymbol, deps, std::move(terminal));
}

std::shared_ptr<gma::INode> buildSimple(const std::string&      symbol,
                                        const std::string&      field,
                                        int                     pollMs,
                                        const Deps&             deps,
                                        std::shared_ptr<gma::INode> terminal) {
  if (field.empty())
    throw std::runtime_error("buildSimple: field is empty");
  if (!deps.store)
    throw std::runtime_error("buildSimple: missing store");

  auto accessor =
    std::make_shared<gma::AtomicAccessor>(symbol, field, deps.store, terminal);

  if (pollMs > 0) {
    gma::rt::ThreadPool* pool = deps.pool;
    if (!pool && gma::gThreadPool)
      pool = gma::gThreadPool.get();
    if (!pool)
      throw std::runtime_error("buildSimple: no thread pool available");
    return std::make_shared<gma::Interval>(std::chrono::milliseconds(pollMs),
                                          accessor,
                                          pool);
  }
  return accessor;

}

// High-level entry: request JSON -> Listener head wired into optional pipeline -> terminal
BuiltChain buildForRequest(const rapidjson::Value&      requestJson,
                           const Deps&                  deps,
                           std::shared_ptr<gma::INode>  terminal) {
  const auto& rq = expectObj(requestJson, "request");

  if (!rq.HasMember("symbol") || !rq["symbol"].IsString())
    throw std::runtime_error("buildForRequest: missing 'symbol'");
  if (!rq.HasMember("field") || !rq["field"].IsString())
    throw std::runtime_error("buildForRequest: missing 'field'");

  const std::string symbol = rq["symbol"].GetString();
  const std::string field  = rq["field"].GetString();

  // Optional mid-pipeline, ultimately forwarding into terminal
  std::shared_ptr<gma::INode> midHead = terminal;

  // Single node under "node"
  if (rq.HasMember("node") && rq["node"].IsObject()) {
    midHead = buildOne(rq["node"], symbol, deps, terminal);
  }

  // Or an array pipeline under "pipeline" or "stages"
  const char* pipeKeys[] = {"pipeline", "stages"};
  for (const char* k : pipeKeys) {
    if (rq.HasMember(k) && rq[k].IsArray()) {
      auto curDown = terminal;
      const auto& arr = rq[k];
      for (int i = static_cast<int>(arr.Size()) - 1; i >= 0; --i) {
        curDown = buildOne(arr[static_cast<rapidjson::SizeType>(i)],
                           symbol,
                           deps,
                           curDown);
      }
      midHead = curDown;
      break;
    }
  }

  if (!deps.dispatcher || !deps.pool)
    throw std::runtime_error("buildForRequest: missing dispatcher/pool");

  using gma::nodes::Listener;
  auto head = std::make_shared<Listener>(symbol,
                                        field,
                                        midHead,
                                        deps.pool,
                                        deps.dispatcher);
  head->start();

  BuiltChain out;
  out.head = head;
  return out;
}

} // namespace gma::tree
