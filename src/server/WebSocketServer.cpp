#include "gma/server/WebSocketServer.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <utility>

using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace websocket = beast::websocket;

namespace gma {

WebSocketServer::WebSocketServer(boost::asio::io_context& ioc,
                                 const tcp::endpoint& endpoint,
                                 SessionFactory session_factory)
  : ioc_(ioc)
  , acceptor_(ioc)
  , endpoint_(endpoint)
  , session_factory_(std::move(session_factory))
  , is_stopped_(false) {}

void WebSocketServer::start() {
  beast::error_code ec;

  acceptor_.open(endpoint_.protocol(), ec);
  if (ec) return;

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) return;

  acceptor_.bind(endpoint_, ec);
  if (ec) return;

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) return;

  doAccept();
}

void WebSocketServer::stop() {
  is_stopped_.store(true);
  beast::error_code ec;
  acceptor_.close(ec);
}

void WebSocketServer::doAccept() {
  if (is_stopped_.load()) return;

  acceptor_.async_accept(
      boost::asio::make_strand(ioc_),
      [this](beast::error_code ec, tcp::socket socket) {
        onAccept(ec, std::move(socket));
      });
}

void WebSocketServer::onAccept(beast::error_code ec, tcp::socket socket) {
  if (!ec) {
    try {
      if (session_factory_) {
        // Let the factory construct and launch the session (shared_ptr)
        auto session = session_factory_(std::move(socket));
        if (session) {
          session->run();
        }
      }
    } catch (...) {
      // swallow to keep server alive
    }
  }
  doAccept(); // accept the next connection no matter what
}

} // namespace gma
