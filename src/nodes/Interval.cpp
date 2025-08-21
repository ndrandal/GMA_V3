#include "gma/nodes/Interval.hpp"
#include <chrono>
#include <thread>

namespace gma {

Interval::Interval(std::chrono::milliseconds delay, std::shared_ptr<INode> child, ThreadPool* pool)
  : _delay(delay), _child(std::move(child)), _pool(pool)
{
  startLoop();
}

void Interval::startLoop() {
  _pool->post([this] {
    while (_running.load()) {
      std::this_thread::sleep_for(_delay);
      if (_child) {
        _child->onValue({"*", 0});  // Symbol is placeholder
      }
    }
  });
}

void Interval::onValue(const SymbolValue&) {
  // No-op: doesn't accept upstream input
}

void Interval::shutdown() {
  running_.store(false, std::memory_order_release);
  if (t_.joinable()) t_.join();
}


} // namespace gma
