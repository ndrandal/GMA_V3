#include "gma/nodes/Aggregate.hpp"

namespace gma {

Aggregate::Aggregate(std::size_t arity, std::shared_ptr<INode> parent)
  : arity_(arity), parent_(std::move(parent)) {}

void Aggregate::onValue(const SymbolValue& sv) {
  std::vector<ArgType> batch;
  {
    std::lock_guard<std::mutex> lk(mx_);
    auto& sb = buf_[sv.symbol];
    sb.vals.push_back(sv.value);
    if (sb.vals.size() < arity_) return;
    batch = std::move(sb.vals);
    sb.vals.clear();
  }

  // Forward outside the lock to avoid holding it during downstream calls
  if (auto p = parent_.lock()) {
    for (const auto& v : batch) {
      p->onValue(SymbolValue{ sv.symbol, v });
    }
  }
}

void Aggregate::shutdown() noexcept {
  std::lock_guard<std::mutex> lk(mx_);
  buf_.clear();
  parent_.reset();
}

} // namespace gma
