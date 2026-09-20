#include "gma/nodes/Expr.hpp"
#include "gma/nodes/NodeEnv.hpp"
#include "gma/util/Logger.hpp"

#include <exception>

namespace gma {

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

  expr::Env env = nodes::envFromValue(sv);

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

  // ENC-1280: an expression rewrites the value, not the bucket it came from.
  ds->onValue(StreamValue{sv.symbol, out, sv.bucketStartMs});
}

void ExprNode::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma
