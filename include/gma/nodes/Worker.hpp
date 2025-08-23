#pragma once
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"

namespace gma {

// A simple "apply(fn(values_for_symbol)) -> one value" node
class Worker final : public INode {
public:
  using Fn = std::function<ArgType(Span<const ArgType>)>;

  Worker(Fn fn, std::shared_ptr<INode> downstream);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  Fn fn_;
  std::weak_ptr<INode> downstream_;

  // Per-symbol accumulation (feed it via Aggregate or by multiple upstreams)
  std::unordered_map<std::string, std::vector<ArgType>> acc_;
};

} // namespace gma
