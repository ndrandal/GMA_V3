#pragma once

#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/RequestRegistry.hpp"
#include <rapidjson/document.h>
#include <functional>
#include <memory>
#include <string>

namespace gma {

class ClientConnection {
public:
  using SendCallback = std::function<void(int key, const SymbolValue&)>;

  ClientConnection(ExecutionContext* ctx, MarketDispatcher* dispatcher, RequestRegistry* registry, SendCallback send);

  void onMessage(const std::string& jsonStr); // called with raw WebSocket message

private:
  ExecutionContext* _ctx;
  MarketDispatcher* _dispatcher;
  RequestRegistry* _registry;
  SendCallback _send;

  std::shared_ptr<INode> createTree(int key, const rapidjson::Value& requestJson);
};

} // namespace gma
