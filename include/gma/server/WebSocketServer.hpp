#include "gma/server/WebSocketServer.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/error.hpp>

namespace gma {

using tcp   = boost::asio::ip::tcp;
namespace beast = boost::beast;

void WebSocketServer::start() {
  beast::error_code ec;

  _acceptor.open(_endpoint.protocol(), ec);
  if (ec) return;

  _acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) return;

  _acceptor.bind(_endpoint, ec);
  if (ec) return;

  _acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) return;

  doAccept();
}

void WebSocketServer::stop() noexcept {
  _stopped.store(true);
  beast::error_code ec;
  _acceptor.close(ec);

  // Optionally walk sessions and stop them
  std::vector<std::shared_ptr<ClientSession>> toStop;
  {
    std::lock_guard<std::mutex> lk(_sessions_mu);
    for (auto it = _sessions.begin(); it != _sessions.end(); ++it) {
      if (auto sp = it->second.lock()) {
        toStop.emplace_back(std::move(sp));
      }
    }
    _sessions.clear();
  }
  for (auto& s : toStop) {
    try { s->stop(); } catch (...) {}
  }
}

void WebSocketServer::doAccept() {
  if (_stopped.load()) return;
  _acceptor.async_accept(
      boost::asio::make_strand(_ioc),
      [this](beast::error_code ec, tcp::socket socket) {
        onAccept(ec, std::move(socket));
      });
}

void WebSocketServer::onAccept(beast::error_code ec, tcp::socket socket) {
  if (!_stopped.load() && !ec) {
    auto session = ClientSession::create(std::move(socket), _ctx, _dispatcher);
    {
      std::lock_guard<std::mutex> lk(_sessions_mu);
      _sessions[session.get()] = session;
    }
    session->run();
  }
  if (!_stopped.load()) {
    doAccept();
  }
}

} // namespace gma
