#include "gma/nodes/AtomicAccessor.hpp"

using namespace gma;

AtomicAccessor::AtomicAccessor(const std::string& symbol,
                               const std::string& field,
                               AtomicStore* store,
                               std::shared_ptr<INode> downstream)
  : _symbol(symbol), _field(field), _store(store), _downstream(downstream) {}

void AtomicAccessor::onValue(const SymbolValue& sv) {
  auto opt = _store->get(_symbol, _field);
  if (opt.has_value()) {
    _downstream->onValue({ _symbol, opt.value() });
  }
}

void AtomicAccessor::shutdown() noexcept {
  _downstream.reset();
}
