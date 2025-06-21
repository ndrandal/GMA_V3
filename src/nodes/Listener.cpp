#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"

using namespace gma::nodes;

Listener::Listener(const std::string& symbol,
                   const std::string& field,
                   std::shared_ptr<INode> downstream,
                   ThreadPool* pool,
                   gma::MarketDispatcher* dispatcher)
  : _symbol(symbol)
  , _field(field)
  , _downstream(std::move(downstream))
  , _pool(pool)
  , _dispatcher(dispatcher)
{
  _dispatcher->registerListener(_symbol, _field, shared_from_this());
}

Listener::~Listener() {
  shutdown();
}

void Listener::onValue(const SymbolValue& sv) {
  // Enqueue the full SymbolValue for later processing
  std::lock_guard lock(_mutex);
  if (_pending.size() >= 1000) _pending.pop();
  _pending.push(sv);
  schedule();
}

void Listener::schedule() {
  bool expected = false;
  if (!_running.compare_exchange_strong(expected, true)) return;

  auto self = shared_from_this();
  _pool->post([self]() {
    while (true) {
      SymbolValue item;
      {
        std::lock_guard lock(self->_mutex);
        if (self->_pending.empty()) break;
        item = std::move(self->_pending.front());
        self->_pending.pop();
      }
      // Dispatch downstream
      if (auto ds = self->_downstream.lock()) {
        ds->onValue(item);
      }
    }
    self->_running = false;
  });
}

void Listener::shutdown() noexcept {
  try {
    _dispatcher->unregisterListener(_symbol, _field, shared_from_this());
  } catch (...) {}
  // Clear pending queue
  std::queue<SymbolValue> empty;
  std::swap(_pending, empty);
}
