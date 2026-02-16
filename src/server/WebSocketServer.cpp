#include "gma/server/WebSocketServer.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>

#include <utility>

// Project headers (adjust paths if your tree differs)
#include "gma/server/ClientSession.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/util/Metrics.hpp"

namespace gma {

using tcp = boost::asio::ip::tcp;

WebSocketServer::WebSocketServer(boost::asio::io_context& ioc,
                                 ExecutionContext* exec,
                                 MarketDispatcher* dispatcher,
                                 unsigned short port)
  : ioc_(ioc),
    acceptor_(ioc),
    exec_(exec),
    dispatcher_(dispatcher)
{
  boost::system::error_code ec;

  tcp::endpoint ep{tcp::v4(), port};
  acceptor_.open(ep.protocol(), ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.bind(ep, ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) throw boost::system::system_error(ec);
}

void WebSocketServer::run() {
  bool expected = false;
  if (!accepting_.compare_exchange_strong(expected, true)) return;
  doAccept();
}

void WebSocketServer::stopAccept() {
  accepting_.store(false);
  boost::system::error_code ec;
  acceptor_.cancel(ec); // ignore ec; cancel pending accept
}

void WebSocketServer::closeAll() {
  std::unordered_map<std::size_t, std::weak_ptr<ClientSession>> copy;
  {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    copy = sessions_;
  }
  for (auto& kv : copy) {
    if (auto sp = kv.second.lock()) {
      sp->close();
    }
  }
}

std::size_t WebSocketServer::registerSession(const std::shared_ptr<ClientSession>& sp) {
  std::size_t id;
  std::size_t count;
  {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    id = next_session_id_++;
    sessions_.emplace(id, sp);
    count = sessions_.size();
  }
  GMA_METRIC_SET("ws.active_connections", static_cast<double>(count));
  return id;
}

void WebSocketServer::unregisterSession(std::size_t id) {
  std::size_t count;
  {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    sessions_.erase(id);
    count = sessions_.size();
  }
  GMA_METRIC_SET("ws.active_connections", static_cast<double>(count));
}

void WebSocketServer::doAccept() {
  if (!accepting_.load()) return;

  acceptor_.async_accept(
    boost::asio::make_strand(ioc_),
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void WebSocketServer::onAccept(boost::system::error_code ec, tcp::socket socket) {
  if (!accepting_.load()) return;

  if (!ec) {
    // Construct a ClientSession that knows how to talk WebSocket/Beast.
    auto session = std::make_shared<ClientSession>(
        std::move(socket),
        this,            // server back-pointer to register/unregister
        exec_,
        dispatcher_);

    // Kick off the session (it will call back registerSession()).
    session->run();
  }
  // Keep accepting regardless of error; owner controls lifetime via stopAccept()
  doAccept();
}

} // namespace gma
