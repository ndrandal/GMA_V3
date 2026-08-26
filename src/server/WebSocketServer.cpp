#include "gma/server/WebSocketServer.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>

#include <string>
#include <utility>

// Project headers (adjust paths if your tree differs)
#include "gma/server/ClientSession.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/util/Metrics.hpp"

namespace gma {

using tcp = boost::asio::ip::tcp;

WebSocketServer::WebSocketServer(boost::asio::io_context& ioc,
                                 ExecutionContext* exec,
                                 Dispatcher* dispatcher,
                                 unsigned short port)
  : ioc_(ioc),
    acceptor_(ioc),
    exec_(exec),
    dispatcher_(dispatcher)
{
  boost::system::error_code ec;

  // ENC-1006: every failure carries the operation and the port it was
  // attempted on, so the message that reaches the operator is actionable
  // ("bind 0.0.0.0:4000") instead of a bare "Address already in use".
  auto fail = [port](const boost::system::error_code& e, const char* op) {
    throw boost::system::system_error(
        e, std::string("websocket server: ") + op + " 0.0.0.0:" + std::to_string(port));
  };

  tcp::endpoint ep{tcp::v4(), port};
  acceptor_.open(ep.protocol(), ec);
  if (ec) fail(ec, "open");

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) fail(ec, "set_option(reuse_address) on");

  acceptor_.bind(ep, ec);
  if (ec) fail(ec, "bind");

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) fail(ec, "listen on");
}

void WebSocketServer::run() {
  bool expected = false;
  if (!accepting_.compare_exchange_strong(expected, true)) return;
  doAccept();
}

unsigned short WebSocketServer::port() const {
  boost::system::error_code ec;
  auto ep = acceptor_.local_endpoint(ec);
  return ec ? 0 : ep.port();
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
    try {
      if (auto sp = kv.second.lock()) {
        sp->close();
      }
    } catch (...) {
      // Don't let one session failure prevent remaining sessions from closing.
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
    // Guard against connection-flood DoS: cap concurrent WS sessions.
    {
      std::lock_guard<std::mutex> lk(sessions_mu_);
      if (sessions_.size() >= MAX_WS_SESSIONS) {
        GMA_METRIC_HIT("ws.conn_rejected");
        // Let socket close on scope exit — don't create a session.
        doAccept();
        return;
      }
    }

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
