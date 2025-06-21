#pragma once

#include "gma/nodes/INode.hpp"
#include "gma/ThreadPool.hpp"
#include <chrono>
#include <memory>
#include <atomic>
#include <thread>

namespace gma {

class Interval final : public INode {
public:
  Interval(std::chrono::milliseconds delay, std::shared_ptr<INode> child, ThreadPool* pool);
  void onValue(const SymbolValue&) override; // unused
  void shutdown() noexcept override;

private:
  void startLoop();

  std::chrono::milliseconds _delay;
  std::shared_ptr<INode> _child;
  ThreadPool* _pool;

  std::atomic<bool> _running{true};
};

} // namespace gma
