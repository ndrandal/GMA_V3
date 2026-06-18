#include "gma/nodes/Expr.hpp"
#include "gma/util/Logger.hpp"

#include <exception>
#include <type_traits>
#include <variant>

namespace gma {
namespace {

// File-local numeric coercion (mirrors TreeBuilder/TumblingWindow toDouble):
// numbers map through; non-numeric values (strings, vectors, nested records)
// collapse to 0.0 so an expression degrades gracefully.
double toDouble(const ArgType& v) {
  return std::visit([](auto&& x) -> double {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, bool>)        return x ? 1.0 : 0.0;
    else if constexpr (std::is_same_v<T, int>)    return static_cast<double>(x);
    else if constexpr (std::is_same_v<T, double>) return x;
    else                                          return 0.0;
  }, v);
}

} // namespace

ExprNode::ExprNode(expr::Compiled fn, std::shared_ptr<INode> downstream)
  : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

void ExprNode::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);
    ds = downstream_;
  }
  if (!ds) return;

  // Build the evaluation environment from the input value.
  expr::Env env;
  if (const Record* r = std::get_if<Record>(&sv.value)) {
    env.reserve(r->fields.size());
    for (const auto& f : r->fields) env[f.name] = toDouble(f.value.value);
  } else {
    env.emplace("value", toDouble(sv.value));
  }

  double out;
  try {
    out = fn_(env);
  } catch (const std::exception& ex) {
    // A FunctionMap leaf (via {"op":"fn"}) threw — drop this tick rather than
    // propagate, matching Worker's fn-exception discipline.
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "expr.eval_exception",
                            {{"symbol", sv.symbol}, {"err", ex.what()}});
    return;
  }

  ds->onValue(StreamValue{sv.symbol, out});
}

void ExprNode::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma
