#include "gma/nodes/Aggregate.hpp"
#include <stdexcept>

namespace gma {

Aggregate::Aggregate(std::size_t arity, std::shared_ptr<INode> parent)
  : arity_(arity), parent_(std::move(parent))
{
  if (arity_ == 0)
    throw std::invalid_argument("Aggregate: arity must be > 0");
}

void Aggregate::onValue(const SymbolValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::vector<ArgType> batch;
  std::shared_ptr<INode> p;
  {
    std::lock_guard<std::mutex> lk(mx_);
    // Cap distinct symbol count to prevent unbounded map growth.
    if (buf_.find(sv.symbol) == buf_.end() && buf_.size() >= MAX_SYMBOLS) {
      return;
    }
    auto& sb = buf_[sv.symbol];
    sb.vals.push_back(sv.value);
    if (sb.vals.size() < arity_) return;
    batch = std::move(sb.vals);
    sb.vals.clear();
    p = parent_;
  }

  // Forward outside the lock to avoid holding it during downstream calls
  if (p) {
    for (const auto& v : batch) {
      p->onValue(SymbolValue{ sv.symbol, v });
    }
  }
}

void Aggregate::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  buf_.clear();
  parent_.reset();
}

} // namespace gma
