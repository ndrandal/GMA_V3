#include "gma/ws/ClientConnection.hpp"
#include "gma/ws/WSResponder.hpp"
#include "gma/ws/JsonSchema.hpp"

// RapidJSON
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "INode.hpp"
// Include your nodes; adjust include paths to your repo:
#include "nodes/AtomicAccessor.hpp"
#include "nodes/Interval.hpp"
#include "nodes/Worker.hpp"
#include "nodes/Listener.hpp"
#include "util/Metrics.hpp"
// If you have a TreeBuilder, include it and use it in buildPipelineFromRequest.

namespace gma::ws {

ClientConnection::ClientConnection(std::string sessionId,
                                   std::shared_ptr<MarketDispatcher> dispatcher,
                                   std::shared_ptr<AtomicStore> store,
                                   SendFn sendText)
: sessionId_(std::move(sessionId)),
  dispatcher_(std::move(dispatcher)),
  store_(std::move(store)),
  sendText_(std::move(sendText)) {}

void ClientConnection::onClose() {
  registry_.removeAll();
}

// ---------- Message entry

void ClientConnection::onTextMessage(const std::string& jsonText){
  rapidjson::Document d;
  if (d.Parse(jsonText.c_str()).HasParseError()) {
    // can't even read "type" safely
    sendText_(std::string("{\"type\":\"error\",\"message\":\"bad JSON: ")
              + rapidjson::GetParseError_En(d.GetParseError()) + "\"}");
    return;
  }
  if (!d.HasMember("type") || !d["type"].IsString()) {
    sendText_("{\"type\":\"error\",\"message\":\"missing 'type'\"}");
    return;
  }
  const std::string type = d["type"].GetString();
  const std::string clientId = d.HasMember("clientId") && d["clientId"].IsString()
                             ? d["clientId"].GetString() : sessionId_;

  if (type == "subscribe") {
    handleSubscribe(clientId, jsonText);
    return;
  }
  if (type == "cancel") {
    handleCancel(clientId, jsonText);
    return;
  }
  if (type == "status") {
    auto snap = gma::util::MetricRegistry::instance().snapshotJson();
    sendText_(std::string("{\"type\":\"status\",\"metrics\":") + snap + "}");
    return;
    }
  // Optional: ping
  if (type == "ping") {
    sendText_(R"({"type":"pong"})");
    return;
  }
  sendText_(R"({"type":"error","message":"unknown type"})");
}

// ---------- subscribe

void ClientConnection::handleSubscribe(const std::string& clientId, const std::string& jsonText){
  rapidjson::Document d; d.Parse(jsonText.c_str());
  if (!d.HasMember("requests") || !d["requests"].IsArray()) {
    sendText_(R"({"type":"error","message":"subscribe missing 'requests'"})");
    return;
  }
  const auto& arr = d["requests"];
  for (auto& req : arr.GetArray()) {
    if (!req.HasMember("id") || !req["id"].IsString()
     || !req.HasMember("symbol") || !req["symbol"].IsString()) {
      sendText_(R"({"type":"error","message":"request missing 'id' or 'symbol'"})");
      continue;
    }
    std::string rid    = req["id"].GetString();
    std::string symbol = req["symbol"].GetString();

    const std::string* field = nullptr;
    int pollMs = 0; const int* pollPtr = nullptr;

    if (req.HasMember("field") && req["field"].IsString()) {
      field = new std::string(req["field"].GetString());
    }
    if (req.HasMember("pollMs") && req["pollMs"].IsInt()) {
      pollMs = req["pollMs"].GetInt(); pollPtr = &pollMs;
    }

    const void* pipelineJson = req.HasMember("pipeline") ? &req["pipeline"] : nullptr;
    const void* operationsJson = req.HasMember("operations") ? &req["operations"] : nullptr;

    try {
      auto root = buildPipelineFromRequest(rid, symbol, field, pollPtr, pipelineJson, operationsJson);
      registry_.add(rid, root);
      // Optionally ack
      std::string ack = std::string("{\"type\":\"ack\",\"id\":\"") + rid + "\"}";
      sendText_(ack);
    } catch (const std::exception& ex) {
      sendError(rid, ex.what());
    } catch (...) {
      sendError(rid, "internal error");
    }
    if (field) delete field;
  }
}

// ---------- cancel

void ClientConnection::handleCancel(const std::string& clientId, const std::string& jsonText){
  rapidjson::Document d; d.Parse(jsonText.c_str());
  if (!d.HasMember("ids") || !d["ids"].IsArray()) {
    sendText_(R"({"type":"error","message":"cancel missing 'ids'"})");
    return;
  }
  for (auto& idv : d["ids"].GetArray()) {
    if (!idv.IsString()) continue;
    std::string rid = idv.GetString();
    registry_.remove(rid);
    std::string ok = std::string("{\"type\":\"canceled\",\"id\":\"")+rid+"\"}";
    sendText_(ok);
  }
}

// ---------- helpers

void ClientConnection::sendError(const std::string& reqId, const std::string& message){
  std::string j = std::string("{\"type\":\"error\",\"id\":\"") + reqId + "\",\"message\":\"" + message + "\"}";
  sendText_(j);
}

// --------------- pipeline builder ---------------

// NOTE: In your repo, if you have a TreeBuilder that can consume `pipeline` JSON, use it here.
// Below is a minimal builder that supports:
//  - Direct AtomicAccessor on a field (px.* or ob.*), optionally polled by Interval
//  - Simple "operations" chain: e.g., [{type:"lastPrice"}, {type:"mean", "period":10}]
std::shared_ptr<INode> ClientConnection::buildPipelineFromRequest(
    const std::string& reqId,
    const std::string& symbol,
    const std::string* fieldOrNull,
    const int* pollMsOrNull,
    const void* pipelineJsonOrNull,
    const void* operationsJsonOrNull)
{
  using gma::ws::WSResponder;

  // Always end with a WSResponder that tags the request id:
  auto responder = std::make_shared<WSResponder>(reqId, sendText_);

  // 1) If explicit field is provided, create AtomicAccessor -> Responder
  if (fieldOrNull) {
    auto accessor = std::make_shared<AtomicAccessor>(symbol, *fieldOrNull); // adjust ctor to your repo
    accessor->setDownstream(responder);

    if (pollMsOrNull && *pollMsOrNull > 0) {
      auto interval = std::make_shared<Interval>(symbol, *pollMsOrNull); // adjust ctor in your repo
      interval->setDownstream(accessor);
      return interval;
    }
    // Push-mode: materializers/dispatchers will notify accessor/responder automatically
    return accessor;
  }

  // 2) If 'operations' array: build a small chain. Example:
  //   [{"type":"lastPrice"}, {"type":"mean","period":10}]
  if (operationsJsonOrNull) {
    const auto& ops = *reinterpret_cast<const rapidjson::Value*>(operationsJsonOrNull);
    if (!ops.IsArray() || ops.Empty()) throw std::runtime_error("'operations' must be a non-empty array");

    // Source: either a Listener on a base series, or an AtomicAccessor to a named atomic.
    std::shared_ptr<INode> head;

    // For simplicity: if first op is "lastPrice" -> Listener on symbol:price
    const auto& op0 = ops[0];
    if (!op0.HasMember("type") || !op0["type"].IsString())
      throw std::runtime_error("operation missing 'type'");
    const std::string t0 = op0["type"].GetString();

    if (t0 == "lastPrice") {
      head = std::make_shared<Listener>(dispatcher_, symbol, "px.last"); // use your field name
    } else {
      // Treat as named atomic, e.g., "px.sma.10" or "ob.spread"
      head = std::make_shared<AtomicAccessor>(symbol, t0);
    }

    std::shared_ptr<INode> cur = head;
    // Remaining ops become Workers (N=1) or other nodes
    for (rapidjson::SizeType i=1; i<ops.Size(); ++i) {
      const auto& op = ops[i];
      if (!op.HasMember("type") || !op["type"].IsString())
        throw std::runtime_error("operation missing 'type'");
      const std::string typ = op["type"].GetString();

      if (typ == "mean") {
        int n = op.HasMember("period") && op["period"].IsInt()? op["period"].GetInt() : 10;
        // Build a Worker that just forwards the mean(n) of upstream values.
        // Since T3 already writes "px.sma.n", prefer using AtomicAccessor for determinism:
        auto acc = std::make_shared<AtomicAccessor>(symbol, std::string("px.sma.")+std::to_string(n));
        cur->setDownstream(acc);
        cur = acc;
      } else if (typ == "ema") {
        int n = op.HasMember("period") && op["period"].IsInt()? op["period"].GetInt() : 20;
        auto acc = std::make_shared<AtomicAccessor>(symbol, std::string("px.ema.")+std::to_string(n));
        cur->setDownstream(acc);
        cur = acc;
      } else if (typ == "vwap") {
        int n = op.HasMember("period") && op["period"].IsInt()? op["period"].GetInt() : 10;
        auto acc = std::make_shared<AtomicAccessor>(symbol, std::string("px.vwap.")+std::to_string(n));
        cur->setDownstream(acc);
        cur = acc;
      } else if (typ == "ob.spread" || typ.rfind("ob.",0)==0) {
        auto acc = std::make_shared<AtomicAccessor>(symbol, typ);
        cur->setDownstream(acc);
        cur = acc;
      } else {
        throw std::runtime_error("unsupported op: "+typ);
      }
    }

    cur->setDownstream(responder);
    // Optional polling:
    if (pollMsOrNull && *pollMsOrNull > 0) {
      auto interval = std::make_shared<Interval>(symbol, *pollMsOrNull);
      interval->setDownstream(head);
      return interval;
    }
    return head;
  }

  // 3) If 'pipeline' object is present and you have TreeBuilder:
  if (pipelineJsonOrNull) {
    // Example (pseudo): auto root = TreeBuilder::build(symbol, *(rapidjson::Value*)pipelineJsonOrNull, responder);
    // return root;
  }

  throw std::runtime_error("request must include either 'field' or 'operations' or 'pipeline'");
}

} // namespace gma::ws
