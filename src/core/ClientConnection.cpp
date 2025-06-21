#include "gma/ClientConnection.hpp"
#include "gma/RequestRegistry.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/JsonValidator.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/nodes/Responder.hpp"

#include <rapidjson/document.h>
#include <iostream>
#include <string>

namespace gma {

ClientConnection::ClientConnection(ExecutionContext* ctx,
                                   MarketDispatcher* dispatcher,
                                   RequestRegistry* registry,
                                   SendCallback send)
  : _ctx(ctx)
  , _dispatcher(dispatcher)
  , _registry(registry)
  , _send(std::move(send))
{}

void ClientConnection::onMessage(const std::string& jsonStr) {
  rapidjson::Document env;
  if (env.Parse(jsonStr.c_str()).HasParseError()) {
    _send(-1, {"*", "Malformed JSON"});
    return;
  }

  if (!env.HasMember("action") || !env["action"].IsString() ||
      !env.HasMember("key")    || !env["key"].IsInt()) {
    _send(-1, {"*", "Missing/invalid 'action' or 'key'"});
    return;
  }

  std::string action = env["action"].GetString();
  int keyInt        = env["key"].GetInt();
  std::string key   = std::to_string(keyInt);

  if (action == "remove") {
    _registry->unregisterRequest(key);
    _registry->unregisterRequest(key + ":responder");
    return;
  }

  if (action == "create") {
    if (!env.HasMember("request") || !env["request"].IsObject()) {
      _send(keyInt, {"*", "Missing/invalid 'request'"});
      return;
    }

    // Copy & validate
    rapidjson::Document req{rapidjson::kObjectType};
    req.CopyFrom(env["request"], req.GetAllocator());
    try {
      JsonValidator::validateRequest(req);
    } catch (const std::exception& ex) {
      _send(keyInt, {"*", std::string("Validation error: ") + ex.what()});
      return;
    }

    // Build the tree: note three arguments now
    std::shared_ptr<INode> root;
    try {
      root = TreeBuilder::build(req, _ctx, _dispatcher);
    } catch (...) {
      std::cerr << "[ClientConnection] Tree build failed\n";
      _send(keyInt, {"*", "Tree build failed"});
      return;
    }

    // Extract symbol/field from validated JSON
    std::string symbol = req["symbol"].GetString();
    std::string field  = req["field"].GetString();

    // Create responder from callback
    auto responder = std::make_shared<nodes::Responder>(_send, keyInt);

    // Register for cleanup: use string keys
    _registry->registerRequest(key, root);
    _registry->registerRequest(key + ":responder", responder);

    // Subscribe responder to ticks
    _dispatcher->registerListener(symbol, field, responder);
    return;
  }

  _send(keyInt, {"*", std::string("Unknown action: ") + action});
}

} // namespace gma
