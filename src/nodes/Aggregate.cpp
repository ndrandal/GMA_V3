// src/nodes/Aggregate.cpp
#include "gma/nodes/Aggregate.hpp"

namespace gma {

Aggregate::Aggregate(std::vector<std::shared_ptr<INode>> children, std::shared_ptr<INode> parent)
  : _children(std::move(children)), _parent(std::move(parent)) {}

void Aggregate::onValue(const SymbolValue& sv) {
  auto& state = _buffer[sv.symbol];
  state.values.push_back(sv.value);
  state.count++;

  if (state.count >= _children.size()) {
    if (auto locked = _parent.lock()) {
      for (const auto& val : state.values) {
        locked->onValue(SymbolValue{sv.symbol, val});
      }
    }
    state.values.clear();
    state.count = 0;
  }

}

void Aggregate::shutdown() noexcept {
  _children.clear();
  _parent.reset();
  _buffer.clear();
}

} // namespace gma
