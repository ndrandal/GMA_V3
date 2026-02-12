#pragma once
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "gma/nodes/INode.hpp"

namespace gma {

class SymbolSplit final : public INode {
public:
  using Factory = std::function<std::shared_ptr<INode>(const std::string& symbol)>;

  explicit SymbolSplit(Factory makeChild);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  mutable std::shared_mutex mx_;
  Factory makeChild_;
  std::unordered_map<std::string, std::shared_ptr<INode>> children_;
};

} // namespace gma
