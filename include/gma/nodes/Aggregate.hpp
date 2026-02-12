#pragma once
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"

namespace gma {

// Fan-in node: collects N values per symbol before forwarding the batch
// to parent. Thread-safe — multiple upstream nodes may call onValue()
// concurrently.
class Aggregate final : public INode {
public:
  Aggregate(std::size_t arity, std::shared_ptr<INode> parent);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  struct SymBuf {
    std::vector<ArgType> vals;
  };

  const std::size_t arity_;
  std::weak_ptr<INode> parent_;

  mutable std::mutex mx_;
  std::unordered_map<std::string, SymBuf> buf_;
};

} // namespace gma
