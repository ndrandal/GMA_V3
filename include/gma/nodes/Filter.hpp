// include/gma/nodes/Filter.hpp
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "gma/nodes/INode.hpp"
#include "gma/Expr.hpp"

namespace gma {

// Predicate gate (ENC-649): forwards each incoming value DOWNSTREAM UNCHANGED
// when a compiled predicate expression over that value is truthy (>0.5), and
// drops it otherwise. The predicate reads the value the same way Expr does — a
// Record exposes its fields by name, a scalar is exposed as "value".
//
// INode::onValue stays void: Filter just conditionally calls downstream — the
// node either forwards or doesn't, it never signals "suppress" up a return
// path. Stateless and bounded (compiled once, O(predicate size) per tick).
class Filter final : public INode {
public:
  Filter(expr::Compiled predicate, std::shared_ptr<INode> downstream);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  expr::Compiled pred_;
  std::shared_ptr<INode> downstream_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
};

} // namespace gma
