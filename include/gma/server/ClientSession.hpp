#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>

namespace gma {

class WebSocketServer;
class ExecutionContext;
class MarketDispatcher;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  using tcp = boost::asio::ip::tcp;
  using Ws  = boost::beast::websocket::stream<boost::beast::tcp_stream>;

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
  void onWrite(boost::beast::error_code ec, std::size_t bytes);

  WebSocketServer*  server_;
  ExecutionContext* exec_;
  MarketDispatcher* dispatcher_;

  Ws ws_;
  boost::beast::flat_buffer buffer_;
  bool open_{false};
  std::size_t sessionId_{0};
};

} // namespace gma
