#include "gma/TreeBuilder.hpp"

#include <stdexcept>
#include <vector>
#include <algorithm>
#include <chrono>

// RapidJSON
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

// Nodes from T2 redo
#include "gma/nodes/Listener.hpp"        // namespace gma::nodes
#include "gma/nodes/Interval.hpp"        // namespace gma
#include "gma/nodes/AtomicAccessor.hpp"  // namespace gma
#include "gma/nodes/Aggregate.hpp"       // namespace gma
#include "gma/nodes/Worker.hpp"          // namespace gma
#include "gma/nodes/SymbolSplit.hpp"     // namespace gma

// ----------------- Tiny helpers -----------------
namespace {

inline const rapidjson::Value& expectObj(const rapidjson::Value& v, const char* what) {
  if (!v.IsObject()) throw std::runtime_error(std::string("TreeBuilder: expected object for ")+what);
  return v;
}
inline const char* expectType(const rapidjson::Value& v) {
  if (!v.HasMember("type") || !v["type"].IsString())
    throw std::runtime_error("TreeBuilder: node missing 'type'");
  return v["type"].GetString();
}
inline std::string strOr(const rapidjson::Value& v, const char* k, const std::string& def="") {
  return (v.HasMember(k) && v[k].IsString()) ? std::string(v[k].GetString()) : def;
}
inline int intOr(const rapidjson::Value& v, const char* k, int def) {
  return (v.HasMember(k) && v[k].IsInt()) ? v[k].GetInt() : def;
}
inline std::size_t sizeOr(const rapidjson::Value& v, const char* k, std::size_t def) {
  return (v.HasMember(k) && v[k].IsUint()) ? static_cast<std::size_t>(v[k].GetUint()) : def;
}
inline bool has(const rapidjson::Value& v, const char* k) { return v.HasMember(k); }

} // namespace

// ----------------- Internal composite root -----------------
// Some graphs have multiple sources (e.g., N Listeners feeding an Aggregate).
// CompositeRoot lets us return a single INode while owning N roots.
namespace {
class CompositeRoot final : public INode {
public:
  explicit CompositeRoot(std::vector<std::shared_ptr<INode>> roots) : roots_(std::move(roots)) {}
  void onValue(const SymbolValue&) override { /* sources don’t need upstream events */ }
  void shutdown() noexcept override {
    for (auto& r : roots_) if (r) r->shutdown();
    roots_.clear();
  }
private:
  std::vector<std::shared_ptr<INode>> roots_;
};
} // namespace

// ----------------- Worker function library -----------------
// NOTE: This is intentionally tiny. TA-style indicators come from AtomicStore (T3).
// Worker is for simple math over a batch (usually paired with Aggregate).
#include <limits>
namespace {
using ArgType = double;
using Span_t  = Span<const ArgType>; // provided by your Worker.hpp include

gma::Worker::Fn fnFromName(const rapidjson::Value& spec) {
  if (!spec.HasMember("fn") || !spec["fn"].IsString())
    throw std::runtime_error("Worker: missing 'fn'");

  const std::string fn = spec["fn"].GetString();

  if (fn == "mean" || fn == "avg") {
    return [](Span_t xs){
      if (xs.size()==0) return 0.0;
      double s=0.0; for (auto v: xs) s += v; return s / xs.size();
    };
  }
  if (fn == "sum") {
    return [](Span_t xs){ double s=0.0; for (auto v: xs) s+=v; return s; };
  }
  if (fn == "max") {
    return [](Span_t xs){ double m=-std::numeric_limits<double>::infinity(); for (auto v: xs) m = std::max(m,v); return (xs.size()?m:0.0); };
  }
  if (fn == "min") {
    return [](Span_t xs){ double m= std::numeric_limits<double>::infinity();  for (auto v: xs) m = std::min(m,v); return (xs.size()?m:0.0); };
  }
  if (fn == "spread") {
    return [](Span_t xs){
      if (xs.size()<2) return 0.0;
      double lo= std::numeric_limits<double>::infinity();
      double hi=-std::numeric_limits<double>::infinity();
      for (auto v: xs) { lo=std::min(lo,v); hi=std::max(hi,v); }
      return hi-lo;
    };
  }
  if (fn == "last") { return [](Span_t xs){ return xs.size()? xs.back():0.0; }; }
  if (fn == "first"){ return [](Span_t xs){ return xs.size()? xs.front():0.0; }; }
  if (fn == "diff") { return [](Span_t xs){ return xs.size()>=2? xs.back()-xs.front():0.0; }; }

  // scale: multiply single input by constant factor
  if (fn == "scale") {
    double factor = spec.HasMember("factor") && spec["factor"].IsNumber() ? spec["factor"].GetDouble() : 1.0;
    return [factor](Span_t xs){ return xs.size()? xs.back()*factor : 0.0; };
  }

  throw std::runtime_error("Worker: unknown fn '"+fn+"'");
}
} // namespace

// ----------------- Builder impl -----------------
namespace gma::tree {

static std::shared_ptr<INode> buildOne(const rapidjson::Value& node,
                                       const std::string& defaultSymbol,
                                       const Deps& deps,
                                       std::shared_ptr<INode> downstream);

// Helper: build array and return a CompositeRoot if >1
static std::shared_ptr<INode> buildManyAsRoot(const rapidjson::Value& arr,
                                              const std::string& defaultSymbol,
                                              const Deps& deps,
                                              std::shared_ptr<INode> downstream)
{
  if (!arr.IsArray()) throw std::runtime_error("TreeBuilder: 'inputs' must be an array");
  std::vector<std::shared_ptr<INode>> roots;
  roots.reserve(arr.Size());
  for (auto& it : arr.GetArray()) {
    roots.push_back(buildOne(it, defaultSymbol, deps, downstream));
  }
  if (roots.empty()) throw std::runtime_error("TreeBuilder: empty 'inputs' array");
  if (roots.size()==1) return roots.front();
  return std::make_shared<CompositeRoot>(std::move(roots));
}

static std::shared_ptr<INode> buildOne(const rapidjson::Value& spec,
                                       const std::string& defaultSymbol,
                                       const Deps& deps,
                                       std::shared_ptr<INode> downstream)
{
  const auto& v   = expectObj(spec, "node");
  const std::string type = expectType(v);

  // --- Listener ---
  if (type == "Listener") {
    if (!deps.dispatcher || !deps.pool) throw std::runtime_error("Listener: missing dispatcher/pool");
    const std::string symbol = strOr(v,"symbol", defaultSymbol);
    const std::string field  = strOr(v,"field",  "");
    if (field.empty()) throw std::runtime_error("Listener: missing 'field'");
    std::size_t cap = sizeOr(v,"queueCap", 1024);
    using gma::nodes::Listener;
    return std::make_shared<Listener>(symbol, field, downstream, deps.pool, deps.dispatcher, cap);
  }

  // --- Interval ---
  if (type == "Interval") {
    if (!deps.pool) throw std::runtime_error("Interval: missing pool");
    int ms = intOr(v, "ms", intOr(v,"periodMs", 0));
    if (ms <= 0) throw std::runtime_error("Interval: positive 'ms' required");
    auto childSpec = v.HasMember("child") ? &v["child"] : nullptr;
    auto child = downstream;
    if (childSpec) child = buildOne(*childSpec, defaultSymbol, deps, downstream);
    return std::make_shared<gma::Interval>(std::chrono::milliseconds(ms), child, deps.pool);
  }

  // --- AtomicAccessor ---
  if (type == "AtomicAccessor") {
    if (!deps.store) throw std::runtime_error("AtomicAccessor: missing store");
    const std::string symbol = strOr(v,"symbol", defaultSymbol);
    const std::string field  = strOr(v,"field",  "");
    if (field.empty()) throw std::runtime_error("AtomicAccessor: missing 'field'");
    return std::make_shared<gma::AtomicAccessor>(symbol, field, deps.store, downstream);
  }

  // --- Worker (batch math over inputs) ---
  if (type == "Worker") {
    auto fn = fnFromName(v);
    return std::make_shared<gma::Worker>(std::move(fn), downstream);
  }

  // --- Aggregate(N) ---
  if (type == "Aggregate") {
    std::size_t arity = sizeOr(v, "arity", 0);
    if (arity == 0) throw std::runtime_error("Aggregate: positive 'arity' required");
    // Create the aggregator which forwards to downstream
    auto agg = std::make_shared<gma::Aggregate>(arity, downstream);
    // Build each input branch to **feed** the aggregate
    if (!v.HasMember("inputs")) throw std::runtime_error("Aggregate: missing 'inputs'");
    auto root = buildManyAsRoot(v["inputs"], defaultSymbol, deps, agg);
    return root; // May be CompositeRoot, which simply owns the input sources.
  }

  // --- SymbolSplit ---
  if (type == "SymbolSplit") {
    if (!v.HasMember("child")) throw std::runtime_error("SymbolSplit: missing 'child'");
    auto childSpec = v["child"]; // template for each symbol
    gma::SymbolSplit::Factory f = [childSpec, defaultSymbol, deps, downstream](const std::string& sym){
      // Build a dedicated subgraph for this symbol, terminating at downstream
      return buildOne(childSpec, sym.empty()?defaultSymbol:sym, deps, downstream);
    };
    return std::make_shared<gma::SymbolSplit>(std::move(f));
  }

  // --- Chain (sequential composition) ---
  if (type == "Chain") {
    if (!v.HasMember("stages") || !v["stages"].IsArray())
      throw std::runtime_error("Chain: 'stages' must be an array");
    auto curDown = downstream;
    // Build from tail to head so each stage points to the one after it
    for (int i = static_cast<int>(v["stages"].Size())-1; i >= 0; --i) {
      curDown = buildOne(v["stages"][static_cast<rapidjson::SizeType>(i)], defaultSymbol, deps, curDown);
    }
    return curDown; // head of the chain
  }

  throw std::runtime_error("TreeBuilder: unknown node type '"+type+"'");
}

// -------- Public API --------

std::shared_ptr<INode> buildNode(const rapidjson::Value& spec,
                                 const std::string& defaultSymbol,
                                 const Deps& deps,
                                 std::shared_ptr<INode> terminal)
{
  return buildOne(spec, defaultSymbol, deps, std::move(terminal));
}

std::shared_ptr<INode> buildSimple(const std::string& symbol,
                                   const std::string& field,
                                   int pollMs,
                                   const Deps& deps,
                                   std::shared_ptr<INode> terminal)
{
  if (field.empty()) throw std::runtime_error("buildSimple: field is empty");
  // Push path: AtomicAccessor -> terminal
  auto accessor = std::make_shared<gma::AtomicAccessor>(symbol, field, deps.store, terminal);
  if (pollMs > 0) {
    // Poll path: Interval -> AtomicAccessor -> terminal
    if (!deps.pool) throw std::runtime_error("buildSimple: missing pool");
    return std::make_shared<gma::Interval>(std::chrono::milliseconds(pollMs), accessor, deps.pool);
  }
  return accessor;
}

} // namespace gma::tree
