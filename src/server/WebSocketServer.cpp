#include "gma/server/WebSocketServer.hpp"
#include "gma/server/ClientSession.hpp"

#include <boost/asio.hpp>
#include <iostream>

namespace gma {
using boost::asio::ip::tcp;

WebSocketServer::WebSocketServer(boost::asio::io_context& ioc,
                                 ExecutionContext* ctx,
                                 MarketDispatcher* dispatcher,
                                 unsigned short port)
  : _acceptor(ioc, tcp::endpoint(tcp::v4(), port)),
    _ctx(ctx),
    _dispatcher(dispatcher)
{}

void WebSocketServer::run() {
  doAccept();
}

void WebSocketServer::doAccept() {
  _acceptor.async_accept(
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void WebSocketServer::onAccept(boost::system::error_code ec, tcp::socket socket) {
  if (ec) {
    std::cerr << "Server accept failed: " << ec.message() << "\n";
  } else {
    std::make_shared<ClientSession>(std::move(socket), _ctx, _dispatcher)->start();
  }

  doAccept(); // accept next
}

} // namespace gma
