// include/gma/nodes/Switch.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>
#include "gma/nodes/INode.hpp"
#include "gma/Expr.hpp"

namespace gma {

// Value-conditioned router (ENC-650): evaluates a compiled selector expression
// over each incoming value to pick an integer index, then forwards the value
// UNCHANGED to exactly one of N case branches — true regime-switching (e.g. a
// selector returning 0/1/2 for bear/flat/bull routes to three pipelines).
//
// The selector value is rounded to the nearest integer. In-range indices route
// to cases[idx]; out-of-range routes to the optional default branch, or drops
// if there is none. The complement of Tee (ENC-646): Tee broadcasts to all
// outputs, Switch routes to one. Like Tee it exclusively owns its branches and
// propagates idempotent shutdown(). Stateless, bounded (one branch per tick).
class Switch final : public INode {
public:
  Switch(expr::Compiled selector,
         std::vector<std::shared_ptr<INode>> cases,
         std::shared_ptr<INode> defaultBranch);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  std::size_t caseCount() const noexcept;

private:
  expr::Compiled sel_;
  std::vector<std::shared_ptr<INode>> cases_;
  std::shared_ptr<INode> default_;  // may be null
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
};

} // namespace gma
