#pragma once
#include <string>
#include "gma/nodes/INode.hpp"
#include "gma/AtomicStore.hpp"

namespace gma {
class AtomicAccessor : public INode {
public:
  AtomicAccessor(const std::string& symbol, const std::string& field, AtomicStore* store, std::shared_ptr<INode> downstream);
  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

private:
  std::string _symbol;
  std::string _field;
  AtomicStore* _store;
  std::shared_ptr<INode> _downstream;
};
}
