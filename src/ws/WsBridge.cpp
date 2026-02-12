#include "gma/ws/WsBridge.hpp"
#include "gma/util/Logger.hpp"

#include <rapidjson/document.h>

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
    // Route to dispatcher/store via the executor context.
    // This is a bridge stub — wire to your ExecutionContext for full functionality.
    gma::util::logger().log(gma::util::LogLevel::Debug,
                            "WsBridge.subscribe",
                            {{"connId", connId}});
    sendTo(connId, R"({"type":"ack","message":"subscribe received"})");
    return;
  }

  if (type == "cancel") {
    gma::util::logger().log(gma::util::LogLevel::Debug,
                            "WsBridge.cancel",
                            {{"connId", connId}});
    sendTo(connId, R"({"type":"ack","message":"cancel received"})");
    return;
  }

  sendTo(connId, R"({"type":"error","message":"unknown type"})");
}

void WsBridge::onClose(const std::string& connId) {
  std::lock_guard<std::mutex> lk(mx_);
  connections_.erase(connId);

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
