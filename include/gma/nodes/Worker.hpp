#pragma once
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"
#include <functional>
#include <vector>
#include <unordered_map>

namespace gma {

class Worker final : public INode {
public:
  using Function = std::function<ArgType(Span<const ArgType>)>;

  Worker(Function fn, std::vector<std::shared_ptr<INode>> children);
  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  struct SymbolState {
    std::vector<ArgType> inputs;
    std::size_t count = 0;
  };

  Function _function;
  std::vector<std::shared_ptr<INode>> _children;
  std::unordered_map<std::string, SymbolState> _buffer;
};

} // namespace gma
