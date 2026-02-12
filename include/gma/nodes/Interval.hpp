#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"

namespace gma {

class Interval final : public INode,
                       public std::enable_shared_from_this<Interval> {
public:
  Interval(std::chrono::milliseconds period,
           std::shared_ptr<INode> child,
           gma::rt::ThreadPool* pool);

  // Must be called after construction when owned by a shared_ptr.
  // Starts the periodic tick loop.
  void start();

  void onValue(const SymbolValue&) override; // no-op
  void shutdown() noexcept override;

private:
  void tickOnce();

  const std::chrono::milliseconds period_;
  std::weak_ptr<INode> child_;
  gma::rt::ThreadPool* pool_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> scheduled_{false};
};

} // namespace gma
