#pragma once
#include <atomic>
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

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  std::string symbol_;
  std::string field_;
  AtomicStore* store_;

  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  std::shared_ptr<INode> downstream_;
};

} // namespace gma
