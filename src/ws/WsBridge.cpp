#include "gma/ws/WsBridge.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/ws/WSResponder.hpp"
#include "gma/util/Logger.hpp"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace gma::ws {

void WsBridge::onOpen(const std::string& connId, SendFn send) {
  std::lock_guard<std::mutex> lk(mx_);
  connections_[connId] = std::move(send);

  gma::util::logger().log(gma::util::LogLevel::Info,
                          "WsBridge.onOpen",
                          {{"connId", connId}});
}

void WsBridge::onMessage(const std::string& connId, const std::string& text) {
  // Parse the incoming JSON message.
  rapidjson::Document doc;
  doc.Parse(text.c_str());

  if (doc.HasParseError() || !doc.IsObject()) {
    sendTo(connId, R"({"type":"error","message":"invalid JSON"})");
    return;
  }

  if (!doc.HasMember("type") || !doc["type"].IsString()) {
    sendTo(connId, R"({"type":"error","message":"missing 'type'"})");
    return;
  }

  const std::string type = doc["type"].GetString();

  if (type == "subscribe") {
    handleSubscribe(connId, doc);
    return;
  }

  if (type == "cancel") {
    handleCancel(connId, doc);
    return;
  }

  sendTo(connId, R"({"type":"error","message":"unknown type"})");
}

void WsBridge::handleSubscribe(const std::string& connId, const rapidjson::Document& doc) {
  if (!doc.HasMember("requests") || !doc["requests"].IsArray()) {
    sendTo(connId, R"({"type":"error","message":"missing 'requests' array"})");
    return;
  }

  for (const auto& r : doc["requests"].GetArray()) {
    if (!r.IsObject()) continue;

    std::string reqId;
    if (r.HasMember("id") && r["id"].IsString()) reqId = r["id"].GetString();
    else if (r.HasMember("id") && r["id"].IsInt()) reqId = std::to_string(r["id"].GetInt());
    else {
      sendTo(connId, R"({"type":"error","message":"request missing 'id'"})");
      continue;
    }

    if (!r.HasMember("symbol") || !r["symbol"].IsString() ||
        !r.HasMember("field") || !r["field"].IsString()) {
      sendTo(connId, R"({"type":"error","message":"request missing 'symbol' or 'field'"})");
      continue;
    }

    const std::string symbol = r["symbol"].GetString();
    const std::string field  = r["field"].GetString();

    // WSResponder serializes SymbolValue to JSON internally; its SendFn
    // just needs to deliver the final JSON string to the connection.
    auto sendCb = [this, connId](const std::string& jsonText) {
      sendTo(connId, jsonText);
    };

    auto terminal = std::make_shared<gma::ws::WSResponder>(reqId, sendCb);

    // Build request JSON for TreeBuilder
    rapidjson::Document rq;
    rq.SetObject();
    auto& a = rq.GetAllocator();
    rq.AddMember("symbol", rapidjson::Value(symbol.c_str(), a), a);
    rq.AddMember("field",  rapidjson::Value(field.c_str(), a), a);

    gma::tree::Deps deps;
    deps.store      = store_.get();
    deps.pool       = pool_ ? pool_ : (gma::gThreadPool ? gma::gThreadPool.get() : nullptr);
    deps.dispatcher = dispatcher_.get();

    try {
      auto built = gma::tree::buildForRequest(rq, deps, terminal);

      {
        std::lock_guard<std::mutex> lk(mx_);
        auto& connSubs = active_[connId];
        auto it = connSubs.find(reqId);
        if (it != connSubs.end() && it->second) {
          it->second->shutdown();
        }
        connSubs[reqId] = built.head;
      }

      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> w(sb);
      w.StartObject();
      w.Key("type"); w.String("subscribed");
      w.Key("id");   w.String(reqId.c_str());
      w.EndObject();
      sendTo(connId, sb.GetString());

    } catch (const std::exception& ex) {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> w(sb);
      w.StartObject();
      w.Key("type");    w.String("error");
      w.Key("message"); w.String(ex.what());
      w.EndObject();
      sendTo(connId, sb.GetString());
    }
  }
}

void WsBridge::handleCancel(const std::string& connId, const rapidjson::Document& doc) {
  if (!doc.HasMember("ids") || !doc["ids"].IsArray()) {
    sendTo(connId, R"({"type":"error","message":"missing 'ids' array"})");
    return;
  }

  for (const auto& v : doc["ids"].GetArray()) {
    std::string reqId;
    if (v.IsString()) reqId = v.GetString();
    else if (v.IsInt()) reqId = std::to_string(v.GetInt());
    else continue;

    std::shared_ptr<gma::INode> head;
    {
      std::lock_guard<std::mutex> lk(mx_);
      auto cit = active_.find(connId);
      if (cit != active_.end()) {
        auto rit = cit->second.find(reqId);
        if (rit != cit->second.end()) {
          head = std::move(rit->second);
          cit->second.erase(rit);
        }
      }
    }
    if (head) head->shutdown();

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type"); w.String("canceled");
    w.Key("id");   w.String(reqId.c_str());
    w.EndObject();
    sendTo(connId, sb.GetString());
  }
}

void WsBridge::onClose(const std::string& connId) {
  std::unordered_map<std::string, std::shared_ptr<gma::INode>> subs;
  {
    std::lock_guard<std::mutex> lk(mx_);
    connections_.erase(connId);
    auto it = active_.find(connId);
    if (it != active_.end()) {
      subs = std::move(it->second);
      active_.erase(it);
    }
  }

  // Shutdown all active subscriptions for this connection
  for (auto& [id, node] : subs) {
    if (node) node->shutdown();
  }

  gma::util::logger().log(gma::util::LogLevel::Info,
                          "WsBridge.onClose",
                          {{"connId", connId}});
}

void WsBridge::sendTo(const std::string& connId, const std::string& msg) {
  std::lock_guard<std::mutex> lk(mx_);
  auto it = connections_.find(connId);
  if (it != connections_.end() && it->second) {
    try {
      it->second(msg);
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "WsBridge.sendTo failed",
                              {{"connId", connId}, {"err", ex.what()}});
    }
  }
}

} // namespace gma::ws
