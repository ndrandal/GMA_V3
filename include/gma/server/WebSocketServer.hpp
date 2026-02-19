#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <boost/asio/ip/tcp.hpp>

namespace gma {

// Forward declarations to keep this header stable
class ExecutionContext;
class MarketDispatcher;

class ClientSession; // forward-declared; defined in gma/server/ClientSession.hpp

/// Simple websocket server wrapper that accepts connections and
/// delegates per-connection work to ClientSession.
class WebSocketServer {
public:
  using tcp = boost::asio::ip::tcp;

  /// Construct a server bound to `port` on all interfaces.
  /// - ioc: external IO context that the owner runs
  /// - exec: optional execution context (can be nullptr)
  /// - dispatcher: optional pointer passed to sessions (can be nullptr)
  WebSocketServer(boost::asio::io_context& ioc,
                  ExecutionContext* exec,
                  MarketDispatcher* dispatcher,
                  unsigned short port);

  WebSocketServer(const WebSocketServer&) = delete;
  WebSocketServer& operator=(const WebSocketServer&) = delete;

  /// Start accepting connections.
  void run();

  /// Stop accepting new connections (does not close existing ones).
  void stopAccept();

  /// Close all active sessions.
  void closeAll();

  /// Called by a session to register itself; returns a numeric id.
  std::size_t registerSession(const std::shared_ptr<ClientSession>& sp);

  /// Called by a session to unregister itself by id.
  void unregisterSession(std::size_t id);

private:
  void doAccept();
  void onAccept(boost::system::error_code ec, tcp::socket socket);

private:
  static constexpr std::size_t MAX_WS_SESSIONS = 1024;

  boost::asio::io_context& ioc_;
  tcp::acceptor            acceptor_;
  std::atomic<bool>        accepting_{false};

  ExecutionContext*        exec_;        // not owned
  MarketDispatcher*        dispatcher_;  // not owned

  std::mutex                                                   sessions_mu_;
  std::unordered_map<std::size_t, std::weak_ptr<ClientSession>> sessions_;
  std::size_t                                                 next_session_id_{1};
};

} // namespace gma
