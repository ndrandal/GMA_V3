#include "gma/nodes/Worker.hpp"

namespace gma {

Worker::Worker(Fn fn, std::shared_ptr<INode> downstream)
  : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

void Worker::onValue(const SymbolValue& sv) {
  auto& vec = acc_[sv.symbol];
  vec.push_back(sv.value);

  // Here we assume batches arrive “naturally” (e.g., via Aggregate):
  // whenever a batch completes (heuristic: empty symbol marks flush; or fixed size),
  // you can decide to compute. Simplest: compute on every push if vector not empty.
  // For deterministic N-ary, prefer wiring: Aggregate(N) -> Worker(fn).
  if (auto ds = downstream_.lock()) {
    ArgType out = fn_(Span<const ArgType>(vec.data(), vec.size()));
    ds->onValue(SymbolValue{ sv.symbol, out });
  }
  vec.clear();
}

void Worker::shutdown() noexcept {
  downstream_.reset();
  acc_.clear();
}

} // namespace gma
