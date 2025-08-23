#include "gma/nodes/AtomicAccessor.hpp"

namespace gma {

AtomicAccessor::AtomicAccessor(std::string symbol,
                               std::string field,
                               AtomicStore* store,
                               std::shared_ptr<INode> downstream)
  : symbol_(std::move(symbol))
  , field_(std::move(field))
  , store_(store)
  , downstream_(std::move(downstream))
{}

void AtomicAccessor::onValue(const SymbolValue& sv) {
  // If caller provided a symbol on the tick, prefer it; else use fixed symbol_
  const std::string& sym = sv.symbol.empty() ? symbol_ : sv.symbol;

  if (!store_) return;
  auto opt = store_->get(sym, field_);
  if (!opt.has_value()) return;

  if (auto ds = downstream_.lock()) {
    ds->onValue(SymbolValue{ sym, opt.value() });
  }
}

void AtomicAccessor::shutdown() noexcept {
  downstream_.reset();
}

} // namespace gma
