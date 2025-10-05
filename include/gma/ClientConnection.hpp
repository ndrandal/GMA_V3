#pragma once
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <memory>
#include <string>

namespace gma { namespace ws {

class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
public:
  using OnMessage = std::function<void(const std::string&)>;

  // New ctor that matches common usage from std::make_shared at call sites.
  ClientConnection(boost::asio::io_context& ioc,
                   std::string host,
                   unsigned short port,
                   std::string target,
                   OnMessage onMessage);

  static std::shared_ptr<ClientConnection>
  create(boost::asio::io_context& ioc,
         std::string host,
         unsigned short port,
         std::string target,
         OnMessage onMessage)
  {
    return std::make_shared<ClientConnection>(ioc, std::move(host), port,
                                              std::move(target), std::move(onMessage));
  }

  void connect();
  void send(std::string text);
  void close();

private:
  void onResolve(boost::beast::error_code ec,
                 boost::asio::ip::tcp::resolver::results_type results);
  void onConnect(boost::beast::error_code ec,
                 boost::asio::ip::tcp::resolver::results_type::endpoint_type ep);
  void onHandshake(boost::beast::error_code ec);
  void doRead();
  void onRead(boost::beast::error_code ec, std::size_t bytes);
  void onWrite(boost::beast::error_code ec, std::size_t bytes);

  boost::asio::io_context& ioc_;
  boost::asio::ip::tcp::resolver resolver_;
  boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
  std::string host_;
  unsigned short port_;
  std::string target_;
  OnMessage onMessage_;
  boost::beast::flat_buffer buffer_;
};

}} // namespace gma::ws
