#pragma once
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>

#include "gma/MarketDispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"

namespace gma::ws {

class WsBridge {
public:
  using SendFn = std::function<void(const std::string&)>; // implement with your socket's send-text

  WsBridge(std::shared_ptr<gma::MarketDispatcher> dispatcher,
           std::shared_ptr<gma::AtomicStore>      store,
           gma::rt::ThreadPool*                    pool = nullptr)
    : dispatcher_(std::move(dispatcher)), store_(std::move(store)), pool_(pool) {}

  // Call these from your WebSocketServer at the right time:
  void onOpen(const std::string& connId, SendFn send);
  void onMessage(const std::string& connId, const std::string& text);
  void onClose(const std::string& connId);

private:
  void sendTo(const std::string& connId, const std::string& msg);
  void handleSubscribe(const std::string& connId, const rapidjson::Document& doc);
  void handleCancel(const std::string& connId, const rapidjson::Document& doc);

  std::mutex mx_;
  std::unordered_map<std::string, SendFn> connections_;

  // Active subscriptions per connection: connId -> (reqId -> head node)
  std::unordered_map<std::string,
    std::unordered_map<std::string, std::shared_ptr<gma::INode>>> active_;

  // Keep intermediate pipeline nodes alive (Listener uses weak_ptr downstream).
  std::unordered_map<std::string,
    std::unordered_map<std::string, std::vector<std::shared_ptr<gma::INode>>>> chains_;

  std::shared_ptr<gma::MarketDispatcher> dispatcher_;
  std::shared_ptr<gma::AtomicStore>      store_;
  gma::rt::ThreadPool*                    pool_ = nullptr;
};

} // namespace gma::ws
