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

  const std::string action = env["action"].GetString();
  const int         keyInt = env["key"].GetInt();
  const std::string key    = std::to_string(keyInt);

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

  // Extract symbol/field from validated JSON
  if (!req.HasMember("symbol") || !req["symbol"].IsString() ||
      !req.HasMember("field")  || !req["field"].IsString()) {
    _send(keyInt, {"*", "Missing 'symbol' or 'field' in request"});
    return;
  }
  const std::string symbol = req["symbol"].GetString();
  const std::string field  = req["field"].GetString();

  // Terminal sender for this request
  auto responder = std::make_shared<nodes::Responder>(_send, keyInt);

  // Compose deps
  gma::tree::Deps deps;
  deps.store      = _ctx ? _ctx->store() : nullptr;
  deps.pool       = _ctx ? _ctx->pool()  : nullptr;
  deps.dispatcher = _dispatcher;

  // Build head -> (optional pipeline) -> responder
  gma::tree::BuiltChain chain;
  try {
    chain = gma::tree::buildForRequest(req, deps, responder);
  } catch (const std::exception& ex) {
    std::cerr << "[ClientConnection] Tree build failed: " << ex.what() << "\n";
    _send(keyInt, {"*", std::string("Tree build failed: ") + ex.what()});
    return;
  } catch (...) {
    std::cerr << "[ClientConnection] Tree build failed (unknown)\n";
    _send(keyInt, {"*", "Tree build failed"});
    return;
  }

  // Registry bookkeeping
  _registry->registerRequest(key, chain.head);
  _registry->registerRequest(key + ":responder", responder);

  // Subscribe the head (NOT the responder)
  _dispatcher->registerListener(symbol, field, chain.head);
  return;
}

} // namespace gma
