#include "gma/nodes/VectorReducer.hpp"
#include "gma/util/Logger.hpp"

#include <utility>
#include <variant>
#include <vector>

namespace gma {

VectorReducer::VectorReducer(Func fn, std::shared_ptr<INode> downstream)
  : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

void VectorReducer::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  // Native input shape is std::vector<double> (what TumblingWindow emits).
  // Anything else (scalar, int, string, etc.) is a wiring mistake — drop
  // with a single Warn line rather than reduce a 1-element synthetic
  // vector that would mask the upstream miswire.
  const auto* vec = std::get_if<std::vector<double>>(&sv.value);
  if (!vec) {
    gma::util::logger().log(gma::util::LogLevel::Warn,
      "VectorReducer: non-vector input dropped",
      {{"symbol", sv.symbol}});
    return;
  }

  double out;
  std::shared_ptr<INode> ds;
  try {
    out = fn_(*vec);
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
      "vector_reducer.fn_exception",
      {{"symbol", sv.symbol}, {"err", ex.what()}});
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mx_);
    ds = downstream_;
  }
  if (ds) {
    ds->onValue(StreamValue{ sv.symbol, ArgType{out} });
  }
}

void VectorReducer::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma
