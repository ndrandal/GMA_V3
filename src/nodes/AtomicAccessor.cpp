#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"

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

void AtomicAccessor::onValue(const SymbolValue&) {
  if (stopping_.load(std::memory_order_acquire)) return;
  if (!store_) return;

  // First check AtomicStore for the field
  auto opt = store_->get(symbol_, field_);

  // If not found in the store, try namespace providers (e.g. "ob.spread")
  if (!opt.has_value()) {
    auto resolved = AtomicProviderRegistry::tryResolve(symbol_, field_);
    if (resolved.has_value()) {
      opt = resolved.value();
    }
  }

  if (!opt.has_value()) return;

  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);
    ds = downstream_;
  }
  if (ds) {
    ds->onValue(SymbolValue{ symbol_, opt.value() });
  }
}

void AtomicAccessor::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
}

} // namespace gma
