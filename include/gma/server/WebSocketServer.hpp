#pragma once

#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <memory>

#include <mutex>
#include <unordered_map>
#include <atomic>


namespace gma {

class WebSocketServer {
public:
  WebSocketServer(boost::asio::io_context& ioc,
                  ExecutionContext* ctx,
                  MarketDispatcher* dispatcher,
                  unsigned short port);

  void run();
  // Graceful shutdown controls
  void stopAccept() noexcept;   // stop taking new connections
  void closeAll() noexcept;     // close all active sessions

  // Session bookkeeping (called by ClientSession)
  void registerSession(const std::shared_ptr<class ClientSession>& s);
  void unregisterSession(class ClientSession* s) noexcept;


private:
  void doAccept();
  void onAccept(boost::system::error_code ec, boost::asio::ip::tcp::socket socket);

  // Accept loop state
  std::atomic<bool> accepting_{false};

  // Live sessions (keyed by raw pointer, values kept weak to avoid cycles)
  mutable std::mutex sessions_mu_;
  std::unordered_map<class ClientSession*, std::weak_ptr<class ClientSession>> sessions_;


  boost::asio::ip::tcp::acceptor _acceptor;
  ExecutionContext* _ctx;
  MarketDispatcher* _dispatcher;
};

} // namespace gma
