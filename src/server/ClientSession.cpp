// File: src/server/ClientSession.cpp

#include "gma/server/ClientSession.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/JsonValidator.hpp"
#include "gma/nodes/Responder.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"
#include "gma/util/JsonUtil.hpp"

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
// Construction / lifecycle
// ------------------------------
ClientSession::ClientSession(tcp::socket socket,
                             WebSocketServer* server,
                             ExecutionContext* exec,
                             Dispatcher* dispatcher)
  : server_(server)
  , exec_(exec)
  , dispatcher_(dispatcher)
, ws_(std::move(socket))
, strand_(ws_.get_executor())
{}

void ClientSession::run() {
  auto self = shared_from_this();

  // Cap incoming WebSocket frame size to prevent memory exhaustion from
  // a malicious client sending a multi-gigabyte message.
  ws_.read_message_max(64 * 1024);  // 64 KB

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

    // Best-effort shutdown of active requests/trees. Shut down EVERY node in
    // each pipeline (head shutdown() does not propagate down the chain), or
    // mid-pipeline timer nodes (TumblingWindow/BucketTime) leak their timer
    // threads forever. Head shutdown() is idempotent, so re-running it here
    // when the head also appears in its keepAlive chain is harmless.
    //
    // ENC-1065 removed Interval from that list: its timer thread no longer
    // holds a reference back to the node, so ~Interval stops and joins it even
    // if shutdown() is never called. TumblingWindow and BucketTime still carry
    // the original pattern, so this sweep is still load-bearing for them.
    {
      std::lock_guard<std::mutex> lk(self->reqMu_);
      for (auto& kv : self->active_) {
        if (kv.second) {
          try { kv.second->shutdown(); } catch (...) {}
        }
      }
      for (auto& kv : self->chains_) {
        for (auto& node : kv.second) {
          if (node) {
            try { node->shutdown(); } catch (...) {}
          }
        }
      }
      self->active_.clear();
      self->chains_.clear();
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
            self->sessionId_ = 0;  // prevent double-unregister
          }
        }
      )
    );
  });
}

// ------------------------------
// Outbound sending (thread-safe)
// ------------------------------
void ClientSession::sendText(std::string s) {
  if (!open_.load()) return;

  auto self = shared_from_this();
  boost::asio::dispatch(strand_, [self, p = std::move(s)]() mutable {
    self->enqueue(std::move(p), nullptr);
  });
}

void ClientSession::sendUpdate(std::string payload,
                               std::uint64_t subId,
                               const std::string& streamKey) {
  if (!open_.load()) return;

  auto self = shared_from_this();
  boost::asio::dispatch(strand_,
    [self, p = std::move(payload), subId, sk = streamKey]() mutable {
      CoalesceKeyView ck{subId, sk};
      self->enqueue(std::move(p), &ck);
    });
}

// Must be called on-strand.
void ClientSession::enqueue(std::string payload, const CoalesceKeyView* ckey) {
  if (!open_.load()) return;

  // ---- 1. Coalesce-latest --------------------------------------------------
  // A pending frame for the same (subscription, streamKey) is superseded by
  // this one, so overwrite it in place instead of appending — but only once the
  // queue is a genuine backlog. Below the watermark every frame is delivered,
  // so a consumer that is keeping up loses nothing.
  if (ckey && outbox_.size() >= COALESCE_WATERMARK) {
    // coalesceIndex_ always points at the NEWEST pending frame for a key (it is
    // re-pointed on every append), so replacing it keeps the queue in value
    // order: older frames for the same key sit ahead of it and still go out
    // first.
    auto it = coalesceIndex_.find(*ckey);
    if (it != coalesceIndex_.end()) {
      const std::uint64_t seq          = it->second;
      const std::uint64_t firstMutable = outboxHeadSeq_ + (writing_ ? 1u : 0u);
      if (seq >= firstMutable && seq < outboxHeadSeq_ + outbox_.size()) {
        outbox_[static_cast<std::size_t>(seq - outboxHeadSeq_)].payload = std::move(payload);
        GMA_METRIC_HIT("ws.outbox_coalesced");
        if (!backpressureLogged_) {
          backpressureLogged_ = true;
          gma::util::logger().log(gma::util::LogLevel::Info,
                                  "ws.outbox_backpressure",
                                  {{"sessionId", std::to_string(sessionId_)},
                                   {"queueSize", std::to_string(outbox_.size())},
                                   {"policy", "coalesce-latest"}});
        }
        return;
      }
      // Either stale (already drained) or the frame currently in flight —
      // Beast holds a buffer pointing straight into that payload string, so it
      // must never be mutated. Fall through and append; the index is re-pointed
      // at the new frame below.
    }
  }

  // ---- 2. Hard memory bound ----------------------------------------------
  if (outbox_.size() >= MAX_OUTBOX_SIZE) {
    if (ckey) {
      // The client is hopelessly behind and already has a pending frame for
      // (nearly) every stream. Drop the newest update instead of the session.
      GMA_METRIC_HIT("ws.outbox_shed");
      return;
    }
    // Lossless frame: make room by shedding the oldest update.
    if (!evictOldestCoalescable()) {
      // Nothing shedable — the queue is pure protocol traffic, which really is
      // unbounded memory growth. Keep the original protection.
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "ws.outbox_overflow",
                              {{"sessionId", std::to_string(sessionId_)},
                               {"queueSize", std::to_string(outbox_.size())}});
      GMA_METRIC_HIT("ws.outbox_overflow");
      close();
      return;
    }
  }

  // ---- 3. Append ---------------------------------------------------------
  const std::uint64_t seq = outboxHeadSeq_ + outbox_.size();
  OutFrame f;
  f.payload = std::move(payload);
  if (ckey) {
    f.key.sub = ckey->sub;
    f.key.streamKey.assign(ckey->streamKey);
  }
  outbox_.push_back(std::move(f));
  if (ckey) coalesceIndex_[outbox_.back().key] = seq;

  if (!writing_) {
    writing_ = true;
    startWrite();
  }
}

// Must be called on-strand.
bool ClientSession::evictOldestCoalescable() {
  // Never touch outbox_.front() while a write is in flight.
  const std::size_t start = writing_ ? 1u : 0u;
  for (std::size_t i = start; i < outbox_.size(); ++i) {
    if (!outbox_[i].coalescable()) continue;
    outbox_.erase(outbox_.begin() + static_cast<std::ptrdiff_t>(i));
    // Erasing from the middle shifts every later frame down one slot, so the
    // recorded sequence numbers are stale. This path only fires when the queue
    // is full of protocol frames (rare, and never the value hot path), so a
    // full rebuild is cheaper to reason about than patching the index.
    reindexOutbox();
    GMA_METRIC_HIT("ws.outbox_shed");
    return true;
  }
  return false;
}

// Must be called on-strand.
void ClientSession::reindexOutbox() {
  coalesceIndex_.clear();
  for (std::size_t i = 0; i < outbox_.size(); ++i) {
    // Ascending order: if two frames share a key (in-flight + queued), the
    // newer one wins, which is exactly the coalescing target we want.
    if (outbox_[i].coalescable()) {
      coalesceIndex_[outbox_[i].key] = outboxHeadSeq_ + i;
    }
  }
}

void ClientSession::startWrite() {
  // This function must be called on-strand.
  if (!open_.load()) {
    writing_ = false;
    outboxHeadSeq_ += outbox_.size();
    outbox_.clear();
    coalesceIndex_.clear();
    return;
  }

  if (outbox_.empty()) {
    writing_ = false;
    return;
  }

  auto self = shared_from_this();

  ws_.text(true);
  ws_.async_write(
    boost::asio::buffer(outbox_.front().payload),
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

  if (!outbox_.empty()) {
    const OutFrame& front = outbox_.front();
    if (front.coalescable()) {
      // Only drop the index entry if it still points at THIS frame: a newer
      // frame for the same key may have been appended while this one was in
      // flight, and that mapping must survive.
      auto it = coalesceIndex_.find(CoalesceKeyView{front.key.sub, front.key.streamKey});
      if (it != coalesceIndex_.end() && it->second == outboxHeadSeq_) {
        coalesceIndex_.erase(it);
      }
    }
    outbox_.pop_front();
    ++outboxHeadSeq_;
  }

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

    // Request key — int (smoke.js / legacy clients) or string
    // (embassy / saved-scene). See gma/server/RequestKey.hpp.
    if (gma::server::requestObjHasBothKeyAndId(r)) {
      sendError("subscribe", "request must have key (int) OR id (int|string), not both");
      continue;
    }
    auto keyOpt = gma::server::parseRequestKeyFromObj(r);
    if (!keyOpt) {
      sendError("subscribe", "request missing valid 'key' (int) or 'id' (int|string)");
      continue;
    }
    gma::server::RequestKey key = std::move(*keyOpt);

    if (!r.HasMember("streamKey") || !r["streamKey"].IsString()) {
      sendError("subscribe", "request missing 'streamKey' string");
      continue;
    }
    if (!r.HasMember("field") || !r["field"].IsString()) {
      sendError("subscribe", "request missing 'field' string");
      continue;
    }

    const std::string streamKey = r["streamKey"].GetString();
    const std::string field     = r["field"].GetString();

    // Validate streamKey/field length to prevent absurdly large map keys.
    static constexpr std::size_t MAX_STREAM_KEY_LEN = 64;
    static constexpr std::size_t MAX_FIELD_LEN      = 128;
    if (streamKey.empty() || streamKey.size() > MAX_STREAM_KEY_LEN) {
      sendError("subscribe", "invalid 'streamKey' (empty or too long, max "
                + std::to_string(MAX_STREAM_KEY_LEN) + ")");
      continue;
    }
    if (field.empty() || field.size() > MAX_FIELD_LEN) {
      sendError("subscribe", "invalid 'field' (empty or too long, max "
                + std::to_string(MAX_FIELD_LEN) + ")");
      continue;
    }

    // Callback from Responder -> send update message over this WS session.
    // Capture weak_ptr to avoid reference cycle:
    //   ClientSession → chains_ → Responder → sendFn → ClientSession
    //
    // subId is this subscription *instance*'s coalescing identity (ENC-996).
    // A re-subscribe on the same request key gets a fresh id, so a straggling
    // frame from the shut-down Responder can never overwrite a new one.
    // handleSubscribe runs on-strand (called from onRead), so the bump is
    // unsynchronised by design.
    const std::uint64_t subId = nextSubId_++;
    auto weak = weak_from_this();
    auto sendFn = [weak, subId](const gma::server::RequestKey& reqKey,
                                const gma::StreamValue& sv) {
      try {
        auto self = weak.lock();
        if (!self) return;

        ::rapidjson::StringBuffer sb;
        ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);

        w.StartObject();
        w.Key("type");   w.String("update");
        gma::server::writeRequestKeyJSON(w, reqKey);
        w.Key("streamKey"); w.String(sv.symbol.c_str());
        w.Key("value");  gma::util::writeArgTypeJson(w, sv.value);
        w.EndObject();

        GMA_METRIC_HIT("ws.msg_out");
        // Value updates are last-value-wins: queue them on the coalescable
        // path, keyed by (subscription instance, streamKey) so a fan-out
        // subscription's streams never supersede each other.
        self->sendUpdate(sb.GetString(), subId, sv.symbol);
      } catch (const std::exception& ex) {
        gma::util::logger().log(gma::util::LogLevel::Error,
          "ws.sendFn exception",
          {{"err", ex.what()}, {"reqKey", gma::server::keyDebugString(reqKey)}});
      }
    };

    std::shared_ptr<gma::INode> terminal =
      std::make_shared<gma::nodes::Responder>(sendFn, key);

    // Build a validated request JSON object to pass to TreeBuilder.
    ::rapidjson::Document rq;
    rq.SetObject();
    auto& a = rq.GetAllocator();

    rq.AddMember("streamKey", ::rapidjson::Value(streamKey.c_str(), a), a);
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

    // Validate pipeline/stages/node sub-trees before building
    try {
      if (r.HasMember("pipeline") && r["pipeline"].IsArray()) {
        for (const auto& elem : r["pipeline"].GetArray()) {
          if (elem.IsObject()) gma::JsonValidator::validateTree(elem);
        }
      }
      if (r.HasMember("stages") && r["stages"].IsArray()) {
        for (const auto& elem : r["stages"].GetArray()) {
          if (elem.IsObject()) gma::JsonValidator::validateTree(elem);
        }
      }
      if (r.HasMember("node") && r["node"].IsObject()) {
        gma::JsonValidator::validateTree(r["node"]);
      }
    } catch (const std::exception& ex) {
      sendError("validate", ex.what());
      continue;
    }

    gma::tree::Deps deps;
    deps.store      = exec_->store();
    deps.pool       = exec_->pool();
    deps.dispatcher = dispatcher_;

    try {
      // Check subscription limit BEFORE building the pipeline to avoid
      // wasting resources and leaking registered listeners.
      {
        std::lock_guard<std::mutex> lk(reqMu_);
        auto it = active_.find(key);
        if (it == active_.end() && active_.size() >= MAX_SUBSCRIPTIONS) {
          sendError("subscribe", "max subscriptions reached");
          continue;
        }
      }

      // Build pipeline OUTSIDE the lock — buildForRequest may be expensive
      // and should not block other subscribe/cancel operations.
      auto built = gma::tree::buildForRequest(rq, deps, terminal);

      {
        std::lock_guard<std::mutex> lk(reqMu_);
        // Replace any existing request with the same key. Shut down the old
        // head AND every node in its keepAlive chain before discarding it —
        // otherwise a replaced mid-pipeline timer node leaks its timer thread.
        auto it = active_.find(key);
        if (it != active_.end() && it->second) {
          it->second->shutdown();
        }
        auto cit = chains_.find(key);
        if (cit != chains_.end()) {
          for (auto& node : cit->second) {
            if (node) {
              try { node->shutdown(); } catch (...) {}
            }
          }
        }
        active_[key] = built.head;
        chains_[key] = std::move(built.keepAlive);
      }

      // Ack
      ::rapidjson::StringBuffer sb;
      ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);
      w.StartObject();
      w.Key("type"); w.String("subscribed");
      gma::server::writeRequestKeyJSON(w, key);
      w.EndObject();

      GMA_METRIC_HIT("ws.subscribe");
      GMA_METRIC_HIT("ws.msg_out");
      sendText(sb.GetString());

      // Log structured field uses "key" or "requestId" mirroring the
      // wire so operators grepping forum's instruction_id can hop
      // straight into gma's view.
      gma::util::logger().log(gma::util::LogLevel::Info,
                              "ws.subscribe",
                              { {gma::server::isInt(key) ? "key" : "requestId",
                                 gma::server::isInt(key)
                                     ? std::to_string(std::get<int>(key))
                                     : std::get<std::string>(key)},
                                {"streamKey", streamKey},
                                {"field", field} });
    } catch (const std::exception& ex) {
      sendError("build", ex.what());
    }
  }
}

void ClientSession::handleCancel(const ::rapidjson::Document& doc) {
  // Accept legacy `keys: [int,...]` AND new `ids: ["...",...]` arrays.
  // Both present in the same payload is a protocol error — keep the
  // parser tight and the failure modes obvious.
  const bool hasKeys = doc.HasMember("keys") && doc["keys"].IsArray();
  const bool hasIds  = doc.HasMember("ids")  && doc["ids"].IsArray();

  if (hasKeys && hasIds) {
    sendError("cancel", "specify keys (int) or ids (string), not both");
    return;
  }
  if (!hasKeys && !hasIds) {
    sendError("cancel", "missing 'keys' (int array) or 'ids' (string array)");
    return;
  }

  // Normalize to a vector<RequestKey>.
  std::vector<gma::server::RequestKey> toCancel;
  if (hasKeys) {
    for (auto& v : doc["keys"].GetArray()) {
      if (!v.IsInt()) {
        sendError("cancel", "keys must be integers");
        continue;
      }
      toCancel.push_back(gma::server::requestKeyInt(v.GetInt()));
    }
  } else {  // hasIds
    for (auto& v : doc["ids"].GetArray()) {
      if (!v.IsString()) {
        sendError("cancel", "ids must be strings");
        continue;
      }
      toCancel.push_back(gma::server::requestKeyStr(v.GetString()));
    }
  }

  for (auto& key : toCancel) {
    std::shared_ptr<gma::INode> root;
    std::vector<std::shared_ptr<gma::INode>> chainVec;
    {
      std::lock_guard<std::mutex> lk(reqMu_);
      auto it = active_.find(key);
      if (it != active_.end()) {
        root = std::move(it->second);
        active_.erase(it);
      }
      auto cit = chains_.find(key);
      if (cit != chains_.end()) {
        chainVec = std::move(cit->second);
        chains_.erase(cit);
      }
    }

    // Shut down the head AND every node in the keepAlive chain (outside the
    // lock) — head shutdown() does not propagate down a linear pipeline, so
    // without this mid-pipeline timer nodes leak their timer threads.
    if (root) root->shutdown();
    for (auto& node : chainVec) {
      if (node) {
        try { node->shutdown(); } catch (...) {}
      }
    }

    ::rapidjson::StringBuffer sb;
    ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type"); w.String("canceled");
    gma::server::writeRequestKeyJSON(w, key);
    w.EndObject();

    GMA_METRIC_HIT("ws.cancel");
    GMA_METRIC_HIT("ws.msg_out");
    sendText(sb.GetString());

    gma::util::logger().log(gma::util::LogLevel::Info,
                            "ws.cancel",
                            { {gma::server::isInt(key) ? "key" : "requestId",
                               gma::server::isInt(key)
                                   ? std::to_string(std::get<int>(key))
                                   : std::get<std::string>(key)} });
  }
}

} // namespace gma
