#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"

using namespace gma::nodes;

Listener::Listener(std::string symbol,
                   std::string field,
                   std::shared_ptr<INode> downstream,
                   gma::rt::ThreadPool* pool,
                   gma::MarketDispatcher* dispatcher,
                   std::size_t queueCap)
  : symbol_(std::move(symbol))
  , field_(std::move(field))
  , downstream_(std::move(downstream))
  , pool_(pool)
  , dispatcher_(dispatcher)
  , q_(queueCap)
{
  if (!dispatcher_) throw std::runtime_error("Listener: dispatcher is null");
  // subscribe this node instance to receive updates for (symbol, field)
  dispatcher_->registerListener(symbol_, field_, shared_from_this());
}

void Listener::onValue(const SymbolValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  // non-blocking enqueue; if full, drop oldest then push to keep latest
  if (!q_.try_push(sv)) {
    if (!q_.drop_one() || !q_.try_push(sv)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    dropped_.fetch_add(1, std::memory_order_relaxed);
  }
  enq_.fetch_add(1, std::memory_order_relaxed);

  // schedule a single pump
  bool expected = false;
  if (scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    schedulePump();
  }
}

void Listener::schedulePump() {
  auto self = shared_from_this();
  pool_->post([self]{
    self->pumpOnce();
  });
}

void Listener::pumpOnce() {
  if (stopping_.load(std::memory_order_acquire)) {
    scheduled_.store(false, std::memory_order_release);
    return;
  }

  constexpr std::size_t kBatch = 512;
  std::size_t processed = 0;
  auto ds = downstream_.lock();

  while (processed < kBatch) {
    auto item = q_.try_pop();
    if (!item) break;
    if (ds) ds->onValue(*item);
    deq_.fetch_add(1, std::memory_order_relaxed);
    ++processed;
  }

  scheduled_.store(false, std::memory_order_release);

  // If more work arrived after we cleared the flag, reschedule.
  if (!stopping_.load(std::memory_order_acquire) && !q_.empty()) {
    bool expected = false;
    if (scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      schedulePump();
    }
  }
}

void Listener::shutdown() noexcept {
  try {
    stopping_.store(true, std::memory_order_release);
    if (dispatcher_) {
      // unsubscribe this exact instance
      dispatcher_->unregisterListener(symbol_, field_, shared_from_this());
    }
  } catch (...) {}

  // drain queue (drop)
  q_.drain([](SymbolValue){});
  downstream_.reset();
}
