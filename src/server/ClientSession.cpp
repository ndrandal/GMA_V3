// File: src/server/ClientSession.cpp

#include "gma/server/ClientSession.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/Responder.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/bind_executor.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <deque>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include <cstdint>

namespace gma {

namespace websocket = boost::beast::websocket;
namespace beast     = boost::beast;
namespace http      = boost::beast::http;

// ------------------------------
// ArgType -> JSON helper writers
// ------------------------------
static void writeArgTypeJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                             const gma::ArgType& v);

static void writeArgValueJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                              const gma::ArgValue& av) {
  writeArgTypeJson(w, av.value);
}

static void writeArgTypeJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                             const gma::ArgType& v) {
  std::visit([&](auto&& x) {
    using T = std::decay_t<decltype(x)>;

    if constexpr (std::is_same_v<T, bool>) {
      w.Bool(x);
    } else if constexpr (std::is_same_v<T, int>) {
      w.Int(x);
    } else if constexpr (std::is_same_v<T, double>) {
      w.Double(x);
    } else if constexpr (std::is_same_v<T, std::string>) {
      w.String(x.c_str());
    } else if constexpr (std::is_same_v<T, std::vector<int>>) {
      w.StartArray();
      for (int n : x) w.Int(n);
      w.EndArray();
    } else if constexpr (std::is_same_v<T, std::vector<double>>) {
      w.StartArray();
      for (double d : x) w.Double(d);
      w.EndArray();
    } else if constexpr (std::is_same_v<T, std::vector<gma::ArgValue>>) {
      w.StartArray();
      for (const auto& it : x) writeArgValueJson(w, it);
      w.EndArray();
    } else {
      // Fallback: unknown variant alternative
      w.Null();
    }
  }, v);
}

// ------------------------------
// Construction / lifecycle
// ------------------------------
ClientSession::ClientSession(tcp::socket socket,
                             WebSocketServer* server,
                             ExecutionContext* exec,
                             MarketDispatcher* dispatcher)
  : server_(server)
  , exec_(exec)
  , dispatcher_(dispatcher)
, ws_(std::move(socket))
, strand_(static_cast<boost::asio::io_context&>(ws_.get_executor().context()).get_executor())
{}

void ClientSession::run() {
  auto self = shared_from_this();

  // Heartbeat: send pings after 30s idle, close if no pong within timeout.
  {
    websocket::stream_base::timeout opt{};
    opt.handshake_timeout = std::chrono::seconds(30);
    opt.idle_timeout      = std::chrono::seconds(30);
    opt.keep_alive_pings  = true;
    ws_.set_option(opt);
  }
  ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(http::field::server,
                std::string(BOOST_BEAST_VERSION_STRING) + " gma-websocket");
      }));

  // Accept + begin read loop on strand.
  ws_.async_accept(
    boost::asio::bind_executor(
      strand_,
      [self](beast::error_code ec) {
        if (ec) {
          gma::util::logger().log(gma::util::LogLevel::Error,
                                  "ws.accept_failed",
                                  { {"err", ec.message()} });
          return;
        }

        self->open_.store(true);
        GMA_METRIC_HIT("ws.accept");

        if (self->server_) {
          self->sessionId_ = self->server_->registerSession(self);
        }

        gma::util::logger().log(gma::util::LogLevel::Info,
                                "ws.accepted",
                                { {"sessionId", std::to_string(self->sessionId_)} });

        self->doRead();
      }
    )
  );
}

void ClientSession::doRead() {
  auto self = shared_from_this();

  ws_.async_read(
    buffer_,
    boost::asio::bind_executor(
      strand_,
      [self](beast::error_code ec, std::size_t bytes) {
        self->onRead(ec, bytes);
      }
    )
  );
}

void ClientSession::onRead(beast::error_code ec, std::size_t) {
  // This handler is on-strand.
  if (ec == websocket::error::closed) {
    close();
    return;
  }
  if (ec) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "ws.read_failed",
                            { {"err", ec.message()} });
    close();
    return;
  }

  const std::string text = beast::buffers_to_string(buffer_.data());
  buffer_.consume(buffer_.size());

  GMA_METRIC_HIT("ws.msg_in");

  handleMessage(text);

  // Continue reading.
  doRead();
}

// ------------------------------
// Close / cleanup
// ------------------------------
void ClientSession::close() {
  auto self = shared_from_this();

  boost::asio::dispatch(strand_, [self]() {
    // idempotent
    if (!self->open_.exchange(false)) return;

    // Best-effort shutdown of active requests/trees
    {
      std::lock_guard<std::mutex> lk(self->reqMu_);
      for (auto& kv : self->active_) {
        if (kv.second) kv.second->shutdown();
      }
      self->active_.clear();
    }

    websocket::close_reason cr;
    cr.code   = websocket::close_code::normal;
    cr.reason = "closing";

    self->ws_.async_close(
      cr,
      boost::asio::bind_executor(
        self->strand_,
        [self](beast::error_code ec) {
          if (ec) {
            gma::util::logger().log(gma::util::LogLevel::Warn,
                                    "ws.close_failed",
                                    { {"err", ec.message()} });
          }

          GMA_METRIC_HIT("ws.close");

          if (self->server_ && self->sessionId_ != 0) {
            self->server_->unregisterSession(self->sessionId_);
          }
        }
      )
    );
  });
}

// ------------------------------
// Outbound sending (thread-safe)
// ------------------------------
void ClientSession::sendText(const std::string& s) {
  if (!open_.load()) return;

  auto self = shared_from_this();
  boost::asio::dispatch(strand_, [self, s]() {
    if (!self->open_.load()) return;

    // Backpressure: if a slow/dead client lets the queue grow too large,
    // close the connection rather than exhausting server memory.
    if (self->outbox_.size() >= MAX_OUTBOX_SIZE) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "ws.outbox_overflow",
                              {{"sessionId", std::to_string(self->sessionId_)},
                               {"queueSize", std::to_string(self->outbox_.size())}});
      self->close();
      return;
    }

    self->outbox_.push_back(s);
    if (!self->writing_) {
      self->writing_ = true;
      self->startWrite();
    }
  });
}

void ClientSession::startWrite() {
  // This function must be called on-strand.
  if (!open_.load()) {
    writing_ = false;
    outbox_.clear();
    return;
  }

  if (outbox_.empty()) {
    writing_ = false;
    return;
  }

  auto self = shared_from_this();

  ws_.text(true);
  ws_.async_write(
    boost::asio::buffer(outbox_.front()),
    boost::asio::bind_executor(
      strand_,
      [self](beast::error_code ec, std::size_t bytes) {
        self->onWrite(ec, bytes);
      }
    )
  );
}

void ClientSession::onWrite(beast::error_code ec, std::size_t) {
  // This handler is on-strand.
  if (ec) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "ws.write_failed",
                            { {"err", ec.message()} });
    // If writes fail, tear down session to prevent stuck state.
    close();
    return;
  }

  if (!outbox_.empty()) outbox_.pop_front();

  // Continue draining queue.
  startWrite();
}

// ------------------------------
// Protocol helpers
// ------------------------------
void ClientSession::sendError(const std::string& where, const std::string& message) {
  ::rapidjson::StringBuffer sb;
  ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);

  w.StartObject();
  w.Key("type");    w.String("error");
  w.Key("where");   w.String(where.c_str());
  w.Key("message"); w.String(message.c_str());
  w.EndObject();

  GMA_METRIC_HIT("ws.msg_out");
  sendText(sb.GetString());
}

void ClientSession::handleMessage(const std::string& text) {
  ::rapidjson::Document doc;
  doc.Parse(text.c_str());

  if (doc.HasParseError() || !doc.IsObject()) {
    sendError("parse", "invalid JSON");
    return;
  }

  if (!doc.HasMember("type") || !doc["type"].IsString()) {
    sendError("type", "missing 'type'");
    return;
  }

  const std::string type = doc["type"].GetString();

  if (type == "subscribe") {
    handleSubscribe(doc);
    return;
  }
  if (type == "cancel") {
    handleCancel(doc);
    return;
  }

  sendError("type", "unknown type: " + type);
}

bool ClientSession::rateLimitCheck() {
  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - rateLastRefill_).count();
  rateLastRefill_ = now;
  rateTokens_ = std::min(static_cast<double>(RATE_LIMIT_BURST),
                          rateTokens_ + elapsed * RATE_LIMIT_PER_SEC);
  if (rateTokens_ < 1.0) return false;
  rateTokens_ -= 1.0;
  return true;
}

void ClientSession::handleSubscribe(const ::rapidjson::Document& doc) {
  if (!exec_ || !dispatcher_) {
    sendError("subscribe", "server missing exec/dispatcher");
    return;
  }

  if (!rateLimitCheck()) {
    sendError("subscribe", "rate limit exceeded");
    return;
  }

  if (!doc.HasMember("requests") || !doc["requests"].IsArray()) {
    sendError("subscribe", "missing 'requests' array");
    return;
  }

  const auto& arr = doc["requests"].GetArray();

  for (auto& r : arr) {
    if (!r.IsObject()) {
      sendError("subscribe", "request must be object");
      continue;
    }

    // Request key (int)
    int key = 0;
    if (r.HasMember("key") && r["key"].IsInt()) key = r["key"].GetInt();
    else if (r.HasMember("id") && r["id"].IsInt()) key = r["id"].GetInt();
    else {
      sendError("subscribe", "request missing integer 'key'");
      continue;
    }

    if (!r.HasMember("symbol") || !r["symbol"].IsString()) {
      sendError("subscribe", "request missing 'symbol' string");
      continue;
    }
    if (!r.HasMember("field") || !r["field"].IsString()) {
      sendError("subscribe", "request missing 'field' string");
      continue;
    }

    const std::string symbol = r["symbol"].GetString();
    const std::string field  = r["field"].GetString();

    // Callback from Responder -> send update message over this WS session.
    auto self = shared_from_this();
    auto sendFn = [self](int reqKey, const gma::SymbolValue& sv) {
      ::rapidjson::StringBuffer sb;
      ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);

      w.StartObject();
      w.Key("type");   w.String("update");
      w.Key("key");    w.Int(reqKey);
      w.Key("symbol"); w.String(sv.symbol.c_str());
      w.Key("value");  writeArgTypeJson(w, sv.value);
      w.EndObject();

      GMA_METRIC_HIT("ws.msg_out");
      self->sendText(sb.GetString());
    };

    std::shared_ptr<gma::INode> terminal =
      std::make_shared<gma::nodes::Responder>(sendFn, key);

    // Build a validated request JSON object to pass to TreeBuilder.
    ::rapidjson::Document rq;
    rq.SetObject();
    auto& a = rq.GetAllocator();

    rq.AddMember("symbol", ::rapidjson::Value(symbol.c_str(), a), a);
    rq.AddMember("field",  ::rapidjson::Value(field.c_str(),  a), a);

    // Optional pass-through: pipeline/stages/node
    if (r.HasMember("pipeline") && r["pipeline"].IsArray()) {
      ::rapidjson::Value pipe(::rapidjson::kArrayType);
      pipe.CopyFrom(r["pipeline"], a);
      rq.AddMember("pipeline", pipe, a);
    }
    if (r.HasMember("stages") && r["stages"].IsArray()) {
      ::rapidjson::Value stages(::rapidjson::kArrayType);
      stages.CopyFrom(r["stages"], a);
      rq.AddMember("stages", stages, a);
    }
    if (r.HasMember("node") && r["node"].IsObject()) {
      ::rapidjson::Value node(::rapidjson::kObjectType);
      node.CopyFrom(r["node"], a);
      rq.AddMember("node", node, a);
    }

    gma::tree::Deps deps;
    deps.store      = exec_->store();
    deps.pool       = exec_->pool();
    deps.dispatcher = dispatcher_;

    try {
      auto built = gma::tree::buildForRequest(rq, deps, terminal);

      {
        std::lock_guard<std::mutex> lk(reqMu_);
        // Replace any existing request with the same key.
        auto it = active_.find(key);
        if (it != active_.end() && it->second) {
          it->second->shutdown();
        } else if (active_.size() >= MAX_SUBSCRIPTIONS) {
          sendError("subscribe", "max subscriptions reached");
          continue;
        }
        active_[key] = built.head;
      }

      // Ack
      ::rapidjson::StringBuffer sb;
      ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);
      w.StartObject();
      w.Key("type"); w.String("subscribed");
      w.Key("key");  w.Int(key);
      w.EndObject();

      GMA_METRIC_HIT("ws.subscribe");
      GMA_METRIC_HIT("ws.msg_out");
      sendText(sb.GetString());

      gma::util::logger().log(gma::util::LogLevel::Info,
                              "ws.subscribe",
                              { {"key", std::to_string(key)},
                                {"symbol", symbol},
                                {"field", field} });
    } catch (const std::exception& ex) {
      sendError("build", ex.what());
    }
  }
}

void ClientSession::handleCancel(const ::rapidjson::Document& doc) {
  if (!doc.HasMember("keys") || !doc["keys"].IsArray()) {
    sendError("cancel", "missing 'keys' array");
    return;
  }

  for (auto& v : doc["keys"].GetArray()) {
    if (!v.IsInt()) {
      sendError("cancel", "keys must be integers");
      continue;
    }

    const int key = v.GetInt();

    std::shared_ptr<gma::INode> root;
    {
      std::lock_guard<std::mutex> lk(reqMu_);
      auto it = active_.find(key);
      if (it != active_.end()) {
        root = std::move(it->second);
        active_.erase(it);
      }
    }

    if (root) root->shutdown();

    ::rapidjson::StringBuffer sb;
    ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type"); w.String("canceled");
    w.Key("key");  w.Int(key);
    w.EndObject();

    GMA_METRIC_HIT("ws.cancel");
    GMA_METRIC_HIT("ws.msg_out");
    sendText(sb.GetString());

    gma::util::logger().log(gma::util::LogLevel::Info,
                            "ws.cancel",
                            { {"key", std::to_string(key)} });
  }
}

} // namespace gma
