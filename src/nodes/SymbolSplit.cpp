#include "gma/nodes/SymbolSplit.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

SymbolSplit::SymbolSplit(Factory makeChild)
  : makeChild_(std::move(makeChild)) {}

void SymbolSplit::onValue(const SymbolValue& sv) {
  if (!makeChild_) return;

  std::shared_ptr<INode> child;
  {
    std::scoped_lock lk(mx_);
    auto it = children_.find(sv.symbol);
    if (it == children_.end()) {
      child = makeChild_(sv.symbol);
      if (!child) return; // factory returned null
      children_.emplace(sv.symbol, child);
    } else {
      child = it->second;
    }
  }
  if (child) child->onValue(sv);
}

void SymbolSplit::shutdown() noexcept {
  std::scoped_lock lk(mx_);
  for (auto& [_, node] : children_) {
    if (node) node->shutdown();
  }
  children_.clear();
}

} // namespace gma
