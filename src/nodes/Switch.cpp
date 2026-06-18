#include "gma/nodes/Switch.hpp"
#include "gma/nodes/NodeEnv.hpp"
#include "gma/util/Logger.hpp"

#include <cmath>
#include <exception>

namespace gma {

Switch::Switch(expr::Compiled selector,
               std::vector<std::shared_ptr<INode>> cases,
               std::shared_ptr<INode> defaultBranch)
  : sel_(std::move(selector)),
    cases_(std::move(cases)),
    default_(std::move(defaultBranch)) {}

void Switch::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  double s;
  try {
    s = sel_(nodes::envFromValue(sv));
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "switch.selector_exception",
                            {{"symbol", sv.symbol}, {"err", ex.what()}});
    return;  // selector threw -> drop
  }

  const long idx = std::lround(s);

  std::shared_ptr<INode> branch;
  {
    std::lock_guard<std::mutex> lk(mx_);
    if (idx >= 0 && static_cast<std::size_t>(idx) < cases_.size())
      branch = cases_[static_cast<std::size_t>(idx)];
    else
      branch = default_;  // null if no default -> drop
  }

  if (branch) branch->onValue(sv);  // route the original value, unchanged
}

void Switch::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);

  std::vector<std::shared_ptr<INode>> branches;
  std::shared_ptr<INode> def;
  {
    std::lock_guard<std::mutex> lk(mx_);
    branches.swap(cases_);
    def.swap(default_);
  }
  for (auto& b : branches)
    if (b) b->shutdown();
  if (def) def->shutdown();
}

std::size_t Switch::caseCount() const noexcept {
  std::lock_guard<std::mutex> lk(mx_);
  return cases_.size();
}

} // namespace gma
