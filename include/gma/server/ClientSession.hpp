#pragma once
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/io_context.hpp>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#include <rapidjson/document.h> // easiest: include real type here (prevents forward-decl mistakes)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gma {

class WebSocketServer;
class ExecutionContext;
class MarketDispatcher;
class INode;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  using tcp = boost::asio::ip::tcp;
  using Ws  = boost::beast::websocket::stream<tcp::socket>;

  ClientSession(tcp::socket socket,
                WebSocketServer* server,
                ExecutionContext* exec,
                MarketDispatcher* dispatcher);

  // Start the WebSocket handshake and begin reading messages.
  void run();

  // Send a text frame to the client (no-op if the session is closed).
  void sendText(const std::string& s);

  // Gracefully close the WebSocket (idempotent).
  void close();

private:
  void doRead();
  void onRead(boost::beast::error_code ec, std::size_t bytes);

  void startWrite();
  void onWrite(boost::beast::error_code ec, std::size_t bytes);

  void handleMessage(const std::string& text);
  void handleSubscribe(const ::rapidjson::Document& doc);
  void handleCancel(const ::rapidjson::Document& doc);
  void sendError(const std::string& where, const std::string& message);

private:
  WebSocketServer*  server_{nullptr};
  ExecutionContext* exec_{nullptr};
  MarketDispatcher* dispatcher_{nullptr};

  Ws ws_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::beast::flat_buffer buffer_;

  std::atomic<bool> open_{false};
  std::uint64_t sessionId_{0};

  // Outbound write serialization (Responder can call sendText from worker threads).
  static constexpr std::size_t MAX_OUTBOX_SIZE = 4096;
  std::deque<std::string> outbox_;
  bool writing_{false};

  // Active requests for this session.
  std::mutex reqMu_;
  std::unordered_map<int, std::shared_ptr<INode>> active_;

  // Rate limiting: token-bucket for subscribe requests
  static constexpr int    RATE_LIMIT_BURST    = 20;   // max burst of subscribes
  static constexpr double RATE_LIMIT_PER_SEC  = 5.0;  // sustained rate
  static constexpr std::size_t MAX_SUBSCRIPTIONS = 256; // max concurrent subscriptions
  double rateTokens_{static_cast<double>(RATE_LIMIT_BURST)};
  std::chrono::steady_clock::time_point rateLastRefill_{std::chrono::steady_clock::now()};
  bool rateLimitCheck();
};

} // namespace gma
