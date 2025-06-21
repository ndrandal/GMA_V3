#pragma once

#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <memory>

namespace gma {

class WebSocketServer {
public:
  WebSocketServer(boost::asio::io_context& ioc,
                  ExecutionContext* ctx,
                  MarketDispatcher* dispatcher,
                  unsigned short port);

  void run();

private:
  void doAccept();
  void onAccept(boost::system::error_code ec, boost::asio::ip::tcp::socket socket);

  boost::asio::ip::tcp::acceptor _acceptor;
  ExecutionContext* _ctx;
  MarketDispatcher* _dispatcher;
};

} // namespace gma
