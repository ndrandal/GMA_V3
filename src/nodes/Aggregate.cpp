#include "gma/nodes/Aggregate.hpp"

namespace gma {

Aggregate::Aggregate(std::size_t arity, std::shared_ptr<INode> parent)
  : arity_(arity), parent_(std::move(parent)) {}

void Aggregate::onValue(const SymbolValue& sv) {
  auto& sb = buf_[sv.symbol];
  sb.vals.push_back(sv.value);
  if (sb.vals.size() < arity_) return;

  if (auto p = parent_.lock()) {
    // Forward the collected vector as multiple onValue calls (simple),
    // or wrap it into a single aggregate value type if you prefer.
    for (const auto& v : sb.vals) {
      p->onValue(SymbolValue{ sv.symbol, v });
    }
  }
  sb.vals.clear();
}

void Aggregate::shutdown() noexcept {
  buf_.clear();
  parent_.reset();
}

} // namespace gma
