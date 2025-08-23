#pragma once
#include <unordered_map>
#include <vector>
#include <memory>
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"

namespace gma {

class Aggregate final : public INode {
public:
  // Fan-in of N children (wired externally); forwards a batch to parent
  Aggregate(std::size_t arity, std::shared_ptr<INode> parent);

  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  struct SymBuf {
    std::vector<ArgType> vals;
  };

  const std::size_t arity_;
  std::weak_ptr<INode> parent_;
  std::unordered_map<std::string, SymBuf> buf_;
};

} // namespace gma
