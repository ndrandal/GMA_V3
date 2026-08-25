#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/io_context.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <rapidjson/document.h> // easiest: include real type here (prevents forward-decl mistakes)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "gma/server/RequestKey.hpp"

namespace gma {

class WebSocketServer;
class ExecutionContext;
class Dispatcher;
class INode;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  using tcp = boost::asio::ip::tcp;
  using Ws  = boost::beast::websocket::stream<tcp::socket>;

  ClientSession(tcp::socket socket,
                WebSocketServer* server,
                ExecutionContext* exec,
                Dispatcher* dispatcher);

  // Start the WebSocket handshake and begin reading messages.
  void run();

  // Send a text frame to the client on the LOSSLESS path (no-op if the session
  // is closed). Protocol frames — acks, errors, cancels — go here: they are
  // never coalesced and never shed.
  void sendText(std::string s);

  // Gracefully close the WebSocket (idempotent).
  void close();

private:
  // --------------------------------------------------------------------
  // Outbound flow control (ENC-996)
  //
  // Outbound frames fall into two classes:
  //   * lossless    — subscribe/cancel acks and errors. These carry protocol
  //                   state; dropping one desynchronises the client.
  //   * coalescable — value updates. A newer value for the same
  //                   (subscription, streamKey) fully supersedes a pending
  //                   one, so the queued frame is REPLACED rather than
  //                   appended. Value streams are last-value-wins by nature;
  //                   a chart that is behind wants the newest sample, not a
  //                   backlog of stale ones.
  //
  // Consequence: once past COALESCE_WATERMARK the queue length is bounded by
  // the number of live (subscription, streamKey) pairs instead of by producer
  // rate, so a producer that outruns the socket degrades to "latest value
  // wins" instead of being disconnected (the pre-ENC-996 policy).
  // --------------------------------------------------------------------
  struct CoalesceKey {
    std::uint64_t sub{0};      // per-session subscription instance id; 0 = lossless
    std::string   streamKey;   // one subscription can fan out over many streams
    bool operator==(const CoalesceKey& o) const noexcept {
      return sub == o.sub && streamKey == o.streamKey;
    }
  };

  // Borrowed view of a CoalesceKey, so the hot path can probe the index
  // without materialising a std::string.
  struct CoalesceKeyView {
    std::uint64_t    sub{0};
    std::string_view streamKey;
  };

  struct CoalesceHash {
    using is_transparent = void;
    static std::size_t mix(std::uint64_t sub, std::string_view sk) noexcept {
      std::size_t h = std::hash<std::string_view>{}(sk);
      h ^= static_cast<std::size_t>(sub) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
    std::size_t operator()(const CoalesceKey& k) const noexcept {
      return mix(k.sub, k.streamKey);
    }
    std::size_t operator()(const CoalesceKeyView& k) const noexcept {
      return mix(k.sub, k.streamKey);
    }
  };

  struct CoalesceEq {
    using is_transparent = void;
    bool operator()(const CoalesceKey& a, const CoalesceKey& b) const noexcept {
      return a.sub == b.sub && a.streamKey == b.streamKey;
    }
    bool operator()(const CoalesceKey& a, const CoalesceKeyView& b) const noexcept {
      return a.sub == b.sub && a.streamKey == b.streamKey;
    }
    bool operator()(const CoalesceKeyView& a, const CoalesceKey& b) const noexcept {
      return a.sub == b.sub && a.streamKey == b.streamKey;
    }
  };

  struct OutFrame {
    std::string payload;
    CoalesceKey key;  // key.sub == 0 → lossless
    bool coalescable() const noexcept { return key.sub != 0; }
  };

  void doRead();
  void onRead(boost::beast::error_code ec, std::size_t bytes);

  // Queue one outbound frame. MUST be called on-strand. `ckey` non-null marks
  // the frame coalescable.
  void enqueue(std::string payload, const CoalesceKeyView* ckey);
  // Drop the oldest coalescable frame (never the one currently in flight) to
  // make room for a lossless frame. Returns false if there is nothing to shed.
  bool evictOldestCoalescable();
  void reindexOutbox();

  // Send a value update on the COALESCABLE path. `subId` is the per-session
  // subscription instance id; `streamKey` disambiguates fan-out subscriptions
  // (e.g. GroupSplit) so one stream's update never supersedes another's.
  void sendUpdate(std::string payload, std::uint64_t subId, const std::string& streamKey);

  void startWrite();
  void onWrite(boost::beast::error_code ec, std::size_t bytes);

  void handleMessage(const std::string& text);
  void handleSubscribe(const ::rapidjson::Document& doc);
  void handleCancel(const ::rapidjson::Document& doc);
  void sendError(const std::string& where, const std::string& message);

private:
  WebSocketServer*  server_{nullptr};
  ExecutionContext* exec_{nullptr};
  Dispatcher* dispatcher_{nullptr};

  Ws ws_;
  // Serialize all async stream ops on the stream's OWN executor (the
  // per-connection strand the socket was accepted on). Beast's keep-alive
  // ping / idle-timeout timers also run on this executor, so binding our
  // reads/writes to it keeps control frames and user writes on one strand.
  Ws::executor_type strand_;
  boost::beast::flat_buffer buffer_;

  std::atomic<bool> open_{false};
  std::uint64_t sessionId_{0};

  // Outbound write serialization (Responder can call sendUpdate from worker
  // threads). All outbox_ / coalesceIndex_ / writing_ access is on-strand.
  //
  // MAX_OUTBOX_SIZE is now a memory bound, not a kill switch: coalescing keeps
  // a well-behaved session far below it, and a session that still reaches it
  // sheds the newest update (or, for a lossless frame, evicts the oldest
  // shedable one). Only a queue made up ENTIRELY of lossless protocol frames
  // can still force a close — that really is unbounded memory growth.
  static constexpr std::size_t MAX_OUTBOX_SIZE = 4096;
  // Low-water mark: below this depth the outbox is a plain lossless FIFO, so a
  // consumer that is keeping up (or only briefly behind) still receives EVERY
  // value — coalescing from depth 1 would silently drop intermediates even for
  // a healthy client, since on a fast link there is essentially always one
  // write in flight. Above it the queue is treated as a backlog and
  // coalesce-latest engages. 256 frames of a single subscription is well under
  // 32 KB, and bounds staleness to 256 samples before degradation starts.
  static constexpr std::size_t COALESCE_WATERMARK = 256;
  std::deque<OutFrame> outbox_;
  // Absolute sequence number of outbox_.front(). coalesceIndex_ maps a frame's
  // coalesce key to its absolute seq, which stays stable as the deque drains
  // (unlike a raw index).
  std::uint64_t outboxHeadSeq_{0};
  std::unordered_map<CoalesceKey, std::uint64_t, CoalesceHash, CoalesceEq> coalesceIndex_;
  bool writing_{false};
  bool backpressureLogged_{false};  // one Info line per session, not per frame

  // Per-session subscription instance counter. Assigned on-strand in
  // handleSubscribe and captured by the Responder's send callback, so two
  // subscriptions (or a re-subscribe on the same request key) never coalesce
  // into each other.
  std::uint64_t nextSubId_{1};

  // Active requests for this session. Variant key supports both
  // smoke.js's int-keyed wire and embassy's string-id wire.
  std::mutex reqMu_;
  std::unordered_map<gma::server::RequestKey, std::shared_ptr<INode>> active_;
  std::unordered_map<gma::server::RequestKey, std::vector<std::shared_ptr<INode>>> chains_; // keeps pipeline alive

  // Rate limiting: token-bucket for subscribe requests
  static constexpr int    RATE_LIMIT_BURST    = 20;   // max burst of subscribes
  static constexpr double RATE_LIMIT_PER_SEC  = 5.0;  // sustained rate
  static constexpr std::size_t MAX_SUBSCRIPTIONS = 256; // max concurrent subscriptions
  double rateTokens_{static_cast<double>(RATE_LIMIT_BURST)};
  std::chrono::steady_clock::time_point rateLastRefill_{std::chrono::steady_clock::now()};
  bool rateLimitCheck();
};

} // namespace gma
