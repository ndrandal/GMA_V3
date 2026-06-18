// include/gma/nodes/Expr.hpp
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "gma/nodes/INode.hpp"
#include "gma/Expr.hpp"

namespace gma {

// Evaluates a compiled expression over each incoming value and forwards the
// scalar result (ENC-645). The input exposes named refs to the expression:
//   - a Record input  -> each field is a ref (by field name)
//   - a scalar input  -> exposed under the ref name "value"
//
// Stateless and bounded: the expression is compiled once at build time and each
// tick does O(expression size) work with no loops/recursion — the engine's
// bounded-cost-per-tick invariant holds. Compose with Pack to assemble the
// record of named inputs an expression reads: Pack -> Expr.
class ExprNode final : public INode {
public:
  ExprNode(expr::Compiled fn, std::shared_ptr<INode> downstream);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  expr::Compiled fn_;
  std::shared_ptr<INode> downstream_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
};

} // namespace gma
