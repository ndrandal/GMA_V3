#pragma once
#include <functional>
#include <string>
#include "gma/nodes/INode.hpp"

namespace gma::ws {

// Sends JSON {type:"update", id, symbol, value, ts} via provided sendText()
class WSResponder : public INode {
public:
  using SendFn = std::function<void(const std::string& jsonText)>;

  WSResponder(std::string reqId, SendFn send)
  : reqId_(std::move(reqId)), send_(std::move(send)) {}

  void onValue(const SymbolValue& v) override;

  void shutdown() override {
    // nothing to tear down; keep for symmetry
  }

private:
  std::string reqId_;
  SendFn send_;
};

} // namespace gma::ws
