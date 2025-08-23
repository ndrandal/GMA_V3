#include "gma/ws/WsBridge.hpp"

namespace gma::ws {

void WsBridge::onOpen(const std::string& connId, SendFn send) {
  std::scoped_lock lk(mx_);
  auto it = sessions_.find(connId);
  if (it != sessions_.end()) return;
  auto client = std::make_shared<ClientConnection>(connId, dispatcher_.get(), store_.get(), std::move(send));
  sessions_.emplace(connId, std::move(client));
}

void WsBridge::onMessage(const std::string& connId, const std::string& text) {
  std::shared_ptr<ClientConnection> c;
  { std::scoped_lock lk(mx_);
    auto it = sessions_.find(connId);
    if (it == sessions_.end()) return;
    c = it->second;
  }
  c->onTextMessage(text);
}

void WsBridge::onClose(const std::string& connId) {
  std::shared_ptr<ClientConnection> c;
  { std::scoped_lock lk(mx_);
    auto it = sessions_.find(connId);
    if (it == sessions_.end()) return;
    c = std::move(it->second);
    sessions_.erase(it);
  }
  if (c) c->onClose();
}

} // namespace gma::ws
