#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "gma/nodes/INode.hpp"
#include "gma/AtomicStore.hpp"

namespace gma {

class AtomicAccessor final : public INode {
public:
  AtomicAccessor(std::string symbol,
                 std::string field,
                 AtomicStore* store,
                 std::shared_ptr<INode> downstream);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  // ENC-1335. See the contract on `INode::samplesAtClock`. This node's output
  // is read from the `AtomicStore` and has nothing to do with the value handed
  // to it, so the read must happen at the clock.
  bool samplesAtClock() const noexcept override { return true; }
  std::function<void()> bindAtClock(const StreamValue& clock) override;

private:
  std::string symbol_;
  std::string field_;
  AtomicStore* store_;

  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  std::shared_ptr<INode> downstream_;
};

} // namespace gma
