#pragma once
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include "gma/ws/ClientConnection.hpp"

namespace gma::ws {

class WsBridge {
public:
  using SendFn = std::function<void(const std::string&)>; // implement with your socket's send-text

  WsBridge(std::shared_ptr<gma::MarketDispatcher> dispatcher,
           std::shared_ptr<gma::AtomicStore> store)
  : dispatcher_(std::move(dispatcher)), store_(std::move(store)) {}

  // Call these from your WebSocketServer at the right time:
  void onOpen(const std::string& connId, SendFn send);
  void onMessage(const std::string& connId, const std::string& text);
  void onClose(const std::string& connId);

private:
  std::mutex mx_;
  std::unordered_map<std::string, std::shared_ptr<ClientConnection>> sessions_;
  std::shared_ptr<gma::MarketDispatcher> dispatcher_;
  std::shared_ptr<gma::AtomicStore> store_;
};

} // namespace gma::ws
