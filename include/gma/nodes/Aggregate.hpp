#pragma once
#include "gma/nodes/INode.hpp"
#include <unordered_map>
#include <vector>

namespace gma {

class Aggregate final : public INode {
public:
  Aggregate(std::vector<std::shared_ptr<INode>> children, std::shared_ptr<INode> parent);
  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  struct SymbolState {
    std::vector<ArgType> values;
    std::size_t count = 0;
  };

  std::vector<std::shared_ptr<INode>> _children;
  std::weak_ptr<INode> _parent;
  std::unordered_map<std::string, SymbolState> _buffer;
};

} // namespace gma
