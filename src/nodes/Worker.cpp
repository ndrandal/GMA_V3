// src/nodes/Worker.cpp
#include "gma/nodes/Worker.hpp"

namespace gma {

Worker::Worker(Function fn, std::vector<std::shared_ptr<INode>> children)
  : _function(std::move(fn)), _children(std::move(children)) {}

void Worker::onValue(const SymbolValue& sv) {
  auto& state = _buffer[sv.symbol];
  state.inputs.push_back(sv.value);
  state.count++;

  if (state.count >= 2) { // TODO: dynamic arity
    ArgType result = _function(Span<const ArgType>(state.inputs.data(), state.inputs.size()));
    for (const auto& child : _children) {
      if (child) child->onValue({sv.symbol, result});
    }
    state.inputs.clear();
    state.count = 0;
  }
}

void Worker::shutdown() noexcept {
  _children.clear();
  _buffer.clear();
}

} // namespace gma
