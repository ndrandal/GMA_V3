#include "gma/nodes/SymbolSplit.hpp"

namespace gma {

SymbolSplit::SymbolSplit(Factory factory)
  : _factory(std::move(factory)) {}

void SymbolSplit::onValue(const SymbolValue& sv) {
  std::shared_ptr<INode> target;

  {
    std::scoped_lock lock(_mutex);
    auto it = _instances.find(sv.symbol);
    if (it != _instances.end()) {
      target = it->second;
    } else {
      target = _factory(sv.symbol);
      _instances[sv.symbol] = target;
    }
  }

  if (target) target->onValue(sv);
}

void SymbolSplit::shutdown() noexcept {
  std::scoped_lock lock(_mutex);
  for (auto& [_, node] : _instances) {
    if (node) node->shutdown();
  }
  _instances.clear();
}

} // namespace gma
