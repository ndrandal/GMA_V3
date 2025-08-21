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

#include "gma/rt/ThreadPool.hpp"

void Listener::onValue(const SymbolValue& v) {
  // Fast, non-blocking enqueue
  if (!q_.try_push(v)) {
    // backpressure: drop oldest and push again (keeps latest)
    if (q_.drop_one() && q_.try_push(v)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
    } else {
      // Worst-case: count a drop and return
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  enq_.fetch_add(1, std::memory_order_relaxed);

  // Schedule one pump if not already scheduled
  bool expected=false;
  if (scheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    gma::gThreadPool->post([self = shared_from_this()]{
      // drain in batches to avoid monopolizing a worker
      constexpr size_t kBatch = 512;
      size_t processed = 0;
      for (;;) {
        size_t n = self->q_.drain([&](SymbolValue sv){
          self->emitDownstream(sv);
        }, kBatch);
        self->deq_.fetch_add(n, std::memory_order_relaxed);
        processed += n;
        if (n < kBatch) break; // queue likely empty
      }
      self->scheduled_.store(false, std::memory_order_release);

      // Race: if new items arrived after we set scheduled_=false, reschedule
      if (!self->q_.empty()) {
        bool expected=false;
        if (self->scheduled_.compare_exchange_strong(expected, true)) {
          gma::gThreadPool->post([self]{ /* tail call: same body */ 
            constexpr size_t kBatch = 512;
            size_t nProcessed=0;
            for (;;) {
              size_t n = self->q_.drain([&](SymbolValue sv){ self->emitDownstream(sv); }, kBatch);
              self->deq_.fetch_add(n, std::memory_order_relaxed);
              nProcessed += n;
              if (n < kBatch) break;
            }
            self->scheduled_.store(false, std::memory_order_release);
            if (!self->q_.empty()) {
              bool e=false; if (self->scheduled_.compare_exchange_strong(e, true)) {
                gma::gThreadPool->post([self]{ /* final resched */ 
                  constexpr size_t kBatch = 512;
                  for (;;) {
                    size_t n = self->q_.drain([&](SymbolValue sv){ self->emitDownstream(sv); }, kBatch);
                    self->deq_.fetch_add(n, std::memory_order_relaxed);
                    if (n < kBatch) break;
                  }
                  self->scheduled_.store(false, std::memory_order_release);
                });
              }
            }
          });
        }
      }
    });
  }
}

void Listener::shutdown() {
  try { dispatcher_->unregisterListener(symbol_, field_, this); } catch (...) {}
  // best-effort drain (optional)
  q_.drain([&](SymbolValue sv){ /* drop */ });
}

