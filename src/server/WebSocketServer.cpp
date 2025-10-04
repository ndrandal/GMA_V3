#include "gma/server/WebSocketServer.hpp"
#include "gma/server/ClientSession.hpp"

#include <boost/asio.hpp>
#include <iostream>

#include <boost/beast/websocket.hpp>
#include <boost/beast/core/error.hpp>

using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;
namespace beast = boost::beast;

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
  accepting_.store(true, std::memory_order_relaxed);
  doAccept();
}

void WebSocketServer::doAccept() {
  _acceptor.async_accept(
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void WebSocketServer::doAccept() {
  if (!accepting_.load(std::memory_order_relaxed)) return;
  acceptor_.async_accept(
      [this](beast::error_code ec, tcp::socket socket) {
        if (!accepting_.load(std::memory_order_relaxed)) return;
        if (ec) {
          // Only continue loop if still accepting
          if (accepting_.load(std::memory_order_relaxed)) {
            doAccept();
          }
          return;
        }
        // construct your ClientSession from socket here (existing code)
        // session->start();
        // and loop:
        if (accepting_.load(std::memory_order_relaxed)) {
          doAccept();
        }
      });
}


void WebSocketServer::registerSession(const std::shared_ptr<ClientSession>& s) {
  if (!s) return;
  std::lock_guard<std::mutex> lk(sessions_mu_);
  sessions_[s.get()] = s;
}

void WebSocketServer::unregisterSession(ClientSession* s) noexcept {
  std::lock_guard<std::mutex> lk(sessions_mu_);
  (void)sessions_.erase(s);
}

void WebSocketServer::stopAccept() noexcept {
  accepting_.store(false, std::memory_order_relaxed);
  beast::error_code ec;
  // Cancel any pending async_accept and close the acceptor; both are idempotent.
  // If acceptor_ is already closed, these are no-ops.
  acceptor_.cancel(ec);
  acceptor_.close(ec);
}

void WebSocketServer::closeAll() noexcept {
  // Snapshot to call stop() without holding the mutex (avoid re-entrancy/deadlocks)
  std::vector<std::shared_ptr<ClientSession>> to_close;
  {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    to_close.reserve(sessions_.size());
    for (auto& kv : sessions_) {
      if (auto sp = kv.second.lock()) {
        to_close.emplace_back(std::move(sp));
      }
    }
  }
  // Politely close each session. Each session will call unregisterSession(this) on close.
  for (auto& s : to_close) {
    try {
      s->stop(); // noexcept
    } catch (...) {
      // Never throw during shutdown.
    }
  }
}

} // namespace gma
