#include "gma/nodes/Filter.hpp"
#include "gma/nodes/NodeEnv.hpp"
#include "gma/util/Logger.hpp"

#include <exception>

namespace gma {

Filter::Filter(expr::Compiled predicate, std::shared_ptr<INode> downstream)
  : pred_(std::move(predicate)), downstream_(std::move(downstream)) {}

void Filter::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);
    ds = downstream_;
  }
  if (!ds) return;

  double verdict;
  try {
    verdict = pred_(nodes::envFromValue(sv));
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "filter.predicate_exception",
                            {{"symbol", sv.symbol}, {"err", ex.what()}});
    return;  // predicate threw -> drop (fail closed)
  }

  if (verdict > 0.5)
    ds->onValue(sv);  // pass the original value through unchanged
}

void Filter::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma
