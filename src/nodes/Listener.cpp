#include "gma/nodes/Listener.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Logger.hpp"

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
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return; // already started

  if (dispatcher_) {
    dispatcher_->registerListener(symbol_, field_, shared_from_this());
  }
}

void Listener::onValue(const gma::SymbolValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::shared_ptr<INode> down;
  {
    std::lock_guard<std::mutex> lk(downMx_);
    down = downstream_.lock();
  }
  if (!down) return;

  if (pool_) {
    pool_->post([d = std::move(down), sym = sv.symbol, val = sv.value]() mutable {
      d->onValue(gma::SymbolValue{std::move(sym), std::move(val)});
    });
  } else {
    down->onValue(sv);
  }
}

void Listener::shutdown() noexcept {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return; // already shutting down

  try {
    if (dispatcher_) {
      dispatcher_->unregisterListener(symbol_, field_, shared_from_this());
    }
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "Listener shutdown error",
                            { {"symbol", symbol_}, {"err", ex.what()} });
  } catch (...) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "Listener shutdown unknown error",
                            { {"symbol", symbol_} });
  }
  {
    std::lock_guard<std::mutex> lk(downMx_);
    downstream_.reset();
  }
}
