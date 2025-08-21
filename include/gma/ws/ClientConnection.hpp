#pragma once
#include <string>
#include <functional>
#include <memory>
#include "gma/ws/RequestRegistry.hpp"

// Forward decls
class INode;
class MarketDispatcher;     // your existing type
class AtomicStore;          // your existing type

namespace gma::ws {

// A per-WS-session glue object
class ClientConnection {
public:
  using SendFn = std::function<void(const std::string&)>;

  ClientConnection(std::string sessionId,
                   std::shared_ptr<MarketDispatcher> dispatcher,
                   std::shared_ptr<AtomicStore> store,
                   SendFn sendText);

  // Call these from your WS server
  void onTextMessage(const std::string& jsonText);
  void onClose();

private:
  void handleSubscribe(const std::string& clientId, const std::string& jsonText);
  void handleCancel(const std::string& clientId, const std::string& jsonText);
  void sendError(const std::string& reqId, const std::string& message);

  // Builders
  std::shared_ptr<INode> buildPipelineFromRequest(const std::string& reqId,
                                                  const std::string& symbol,
                                                  const std::string* fieldOrNull,
                                                  const int* pollMsOrNull,
                                                  const void* pipelineJsonOrNull, // optional
                                                  const void* operationsJsonOrNull // optional
                                                  );

private:
  std::string sessionId_;
  std::shared_ptr<MarketDispatcher> dispatcher_;
  std::shared_ptr<AtomicStore> store_;
  SendFn sendText_;
  RequestRegistry registry_;
};

} // namespace gma::ws
