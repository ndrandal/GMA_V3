#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"

using namespace gma::nodes;

Listener::Listener(std::string symbol,
                   std::string field,
                   std::shared_ptr<INode> downstream,
                   gma::rt::ThreadPool* pool,
                   gma::MarketDispatcher* dispatcher)
  : symbol_(std::move(symbol))
  , field_(std::move(field))
  , downstream_(std::move(downstream))
  , pool_(pool)
  , dispatcher_(dispatcher)
{
}

void Listener::start() {
  // Must be called after construction when owned by a shared_ptr.
  if (dispatcher_) {
    dispatcher_->registerListener(symbol_, field_, shared_from_this());
  }
}

void Listener::onValue(const gma::SymbolValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  auto down = downstream_.lock();
  if (!down) return;

  if (pool_) {
    gma::SymbolValue copy = sv;
    pool_->post([d = std::move(down), v = std::move(copy)]() mutable {
      d->onValue(v);
    });
  } else {
    down->onValue(sv);
  }
}

void Listener::shutdown() noexcept {
  try {
    stopping_.store(true, std::memory_order_release);
    if (dispatcher_) {
      dispatcher_->unregisterListener(symbol_, field_, shared_from_this());
    }
  } catch (...) {}
  downstream_.reset();
}
