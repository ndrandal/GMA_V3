#pragma once

#include "gma/nodes/INode.hpp"
#include <unordered_map>
#include <functional>
#include <memory>
#include <mutex>

namespace gma {

class SymbolSplit final : public INode {
public:
  using Factory = std::function<std::shared_ptr<INode>(const std::string& symbol)>;

  explicit SymbolSplit(Factory factory);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  std::mutex _mutex;
  Factory _factory;
  std::unordered_map<std::string, std::shared_ptr<INode>> _instances;
};

} // namespace gma
