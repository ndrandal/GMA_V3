#pragma once
#include <memory>
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
  std::weak_ptr<INode> downstream_;
};

} // namespace gma
