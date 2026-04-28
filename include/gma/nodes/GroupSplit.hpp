#pragma once
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "gma/nodes/INode.hpp"

namespace gma {

// Fans an incoming StreamValue out to a per-key child node, constructed on first use.
class GroupSplit final : public INode {
public:
  using Factory = std::function<std::shared_ptr<INode>(const std::string& streamKey)>;

  explicit GroupSplit(Factory makeChild);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  static constexpr std::size_t MAX_CHILDREN = 10000;
  mutable std::shared_mutex mx_;
  Factory makeChild_;
  std::unordered_map<std::string, std::shared_ptr<INode>> children_;
};

} // namespace gma
