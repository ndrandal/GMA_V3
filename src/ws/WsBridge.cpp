#include "gma/ws/WsBridge.hpp"

namespace gma::ws {

void WsBridge::onOpen(const std::string& connId, SendFn send) {
  std::lock_guard<std::mutex> lk(mx_);
  connections_[connId] = std::move(send);
}

void WsBridge::onMessage(const std::string& connId, const std::string& text) {
  (void)connId;
  (void)text;
  // TODO: glue JSON text -> ExecutionContext/MarketDispatcher using dispatcher_/store_.
}

void WsBridge::onClose(const std::string& connId) {
  std::lock_guard<std::mutex> lk(mx_);
  connections_.erase(connId);
}

} // namespace gma::ws
