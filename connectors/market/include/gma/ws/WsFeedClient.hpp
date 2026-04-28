#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>

#ifdef GMA_HAS_SSL
#include <boost/asio/ssl.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket/ssl.hpp>
#endif

#include "gma/feed/FeedEvent.hpp"

namespace gma {

class Dispatcher;
class OrderBookManager;

namespace feed { class IFeedAdapter; }

namespace ws {

/// Connects to an external WebSocket feed, delegates message translation to a
/// pluggable IFeedAdapter, and routes the resulting FeedEvents to
/// Dispatcher + OrderBookManager.
///
/// Supports ws:// (plain) and wss:// (TLS, requires GMA_HAS_SSL).
/// Auto-reconnects with exponential backoff on disconnection.
class WsFeedClient : public std::enable_shared_from_this<WsFeedClient> {
public:
  WsFeedClient(boost::asio::io_context& ioc,
               Dispatcher* dispatcher,
               OrderBookManager* obManager,
               const std::string& url,
               std::unique_ptr<feed::IFeedAdapter> adapter,
               std::vector<std::string> symbols = {"*"});

  void start();
  void stop();

private:
  // ---- URL parsing ----
  struct Url {
    bool tls = false;
    std::string host;
    std::string port;
    std::string target;
  };
  static Url parseUrl(const std::string& url);

  // ---- Connection lifecycle ----
  void doResolve();
  void onResolve(boost::beast::error_code ec,
                 boost::asio::ip::tcp::resolver::results_type results);
  void onTcpConnect(boost::beast::error_code ec,
                    boost::asio::ip::tcp::resolver::results_type::endpoint_type ep);
  void onSslHandshake(boost::beast::error_code ec);
  void onWsHandshake(boost::beast::error_code ec);

  // ---- Read loop ----
  void doRead();
  void onRead(boost::beast::error_code ec, std::size_t bytes);

  // ---- Reconnect ----
  void scheduleReconnect();
  void resetStreams();

  // ---- Error ----
  void fail(boost::beast::error_code ec, const char* where);

  // ---- Message handling ----
  void handleMessage(const std::string& text);
  void dispatchEvent(feed::FeedEvent& evt);

  // ---- ASIO plumbing ----
  boost::asio::io_context& ioc_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::asio::steady_timer reconnectTimer_;

  // Plain WS stream
  using PlainWs = boost::beast::websocket::stream<boost::beast::tcp_stream>;
  std::unique_ptr<PlainWs> ws_;

#ifdef GMA_HAS_SSL
  // TLS WS stream
  boost::asio::ssl::context sslCtx_;
  using SslWs = boost::beast::websocket::stream<
      boost::beast::ssl_stream<boost::beast::tcp_stream>>;
  std::unique_ptr<SslWs> wss_;
#endif

  boost::beast::flat_buffer buffer_;

  // ---- Core dependencies (not owned) ----
  Dispatcher* dispatcher_;
  OrderBookManager* obManager_;

  // ---- Feed adapter (protocol translation) ----
  std::unique_ptr<feed::IFeedAdapter> adapter_;

  // ---- Config ----
  Url url_;
  std::vector<std::string> subscribeSymbols_;

  // ---- Lifecycle ----
  std::atomic<bool> stopping_{false};
  int reconnectAttempts_{0};
  static constexpr int MAX_RECONNECT_DELAY_SEC = 30;
};

} // namespace ws
} // namespace gma
