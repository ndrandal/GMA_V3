#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"

namespace gma {

// Applies fn to accumulated per-symbol values and forwards the result.
// Computes on every incoming value (accumulator contains all values seen so far
// for that symbol since last clear). For deterministic N-ary batching, wire
// Aggregate(N) upstream.
class Worker final : public INode {
public:
  using Fn = std::function<ArgType(Span<const ArgType>)>;

  Worker(Fn fn, std::shared_ptr<INode> downstream);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  Fn fn_;
  std::weak_ptr<INode> downstream_;

  mutable std::mutex mx_;
  std::unordered_map<std::string, std::vector<ArgType>> acc_;
};

} // namespace gma
