#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>

namespace gma {

class ClientSession; // fwd-declare your session type

class WebSocketServer {
public:
  using tcp   = boost::asio::ip::tcp;
  using error_code = boost::system::error_code;

  WebSocketServer(boost::asio::io_context& ioc, const tcp::endpoint& endpoint)
  : _ioc(ioc), _endpoint(endpoint), _acceptor(ioc), _stopped(false) {}

  // Factory setter so server can construct sessions from accepted sockets
  template<typename F>
  void set_session_factory(F&& f) { session_factory_ = std::forward<F>(f); }

  void start();
  void stop();

private:
  void doAccept();

  boost::asio::io_context& _ioc;
  tcp::endpoint            _endpoint;
  tcp::acceptor            _acceptor;
  std::atomic<bool>        _stopped;

  std::mutex _sessions_mu;
  std::unordered_map<void*, std::shared_ptr<ClientSession>> _sessions;

  // function: std::shared_ptr<ClientSession>(tcp::socket&&)
  std::function<std::shared_ptr<ClientSession>(tcp::socket&&)> session_factory_;
};

} // namespace gma
