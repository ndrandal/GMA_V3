#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>

namespace gma {

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  using Ws = boost::beast::websocket::stream<boost::beast::tcp_stream>;

  explicit ClientSession(boost::asio::io_context& ioc);
  void start();
  void sendText(const std::string& s);

  // NEW: close() so WebSocketServer.cpp can call it.
  void close();

private:
  void doRead();
  void onRead(boost::beast::error_code ec, std::size_t);
  void onWrite(boost::beast::error_code ec, std::size_t);

  boost::asio::io_context& ioc_;
  Ws ws_;
  boost::beast::flat_buffer buffer_;
  bool textMode_ = true;
};

} // namespace gma
