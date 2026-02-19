#include "gma/nodes/SymbolSplit.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

SymbolSplit::SymbolSplit(Factory makeChild)
  : makeChild_(std::move(makeChild)) {}

void SymbolSplit::onValue(const SymbolValue& sv) {
  if (!makeChild_) return;

  // Fast path: shared_lock for the common case (symbol already exists)
  std::shared_ptr<INode> child;
  {
    std::shared_lock lk(mx_);
    auto it = children_.find(sv.symbol);
    if (it != children_.end()) {
      child = it->second;
    }
  }

  // Slow path: upgrade to unique_lock for first-time symbol insertion
  if (!child) {
    std::unique_lock lk(mx_);
    // Double-check under exclusive lock
    auto [it, inserted] = children_.emplace(sv.symbol, nullptr);
    if (inserted) {
      try {
        it->second = makeChild_(sv.symbol);
      } catch (...) {
        // If makeChild_ throws, remove the nullptr entry so the symbol
        // can be retried on the next call rather than being permanently null.
        children_.erase(it);
        throw;
      }
      if (!it->second) {
        children_.erase(it);
        return;
      }
    }
    child = it->second;
  }

  if (child) child->onValue(sv);
}

void SymbolSplit::shutdown() noexcept {
  std::unique_lock lk(mx_);
  for (auto& [_, node] : children_) {
    if (node) node->shutdown();
  }
  children_.clear();
}

} // namespace gma
