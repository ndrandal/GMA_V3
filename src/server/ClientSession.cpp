#include "gma/server/ClientSession.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"

#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/buffer.hpp>

#include <iostream>

namespace gma {

namespace websocket = boost::beast::websocket;
namespace beast     = boost::beast;
namespace http      = boost::beast::http;

ClientSession::ClientSession(tcp::socket socket,
                             WebSocketServer* server,
                             ExecutionContext* exec,
                             MarketDispatcher* dispatcher)
  : server_(server)
  , exec_(exec)
  , dispatcher_(dispatcher)
  , ws_(std::move(socket))
{}

void ClientSession::run() {
  auto self = shared_from_this();

  ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  ws_.set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(http::field::server,
                std::string(BOOST_BEAST_VERSION_STRING) + " gma-websocket");
      }));

  ws_.async_accept(
      [self](beast::error_code ec) {
        if (ec) {
          std::cerr << "[ClientSession] accept: " << ec.message() << "\n";
          return;
        }

        self->open_ = true;
        if (self->server_) {
          self->sessionId_ = self->server_->registerSession(self);
        }
        self->doRead();
      });
}

void ClientSession::sendText(const std::string& s) {
  if (!open_) return;

  auto self = shared_from_this();
  self->ws_.text(true);
  self->ws_.async_write(
      boost::asio::buffer(s),
      [self](beast::error_code ec, std::size_t bytes) {
        self->onWrite(ec, bytes);
      });
}

void ClientSession::close() {
  if (!open_) return;
  open_ = false;

  auto self = shared_from_this();
  websocket::close_reason cr;
  cr.code   = websocket::close_code::normal;
  cr.reason = "closing";

  ws_.async_close(
      cr,
      [self](beast::error_code ec) {
        if (ec) {
          std::cerr << "[ClientSession] close: " << ec.message() << "\n";
        }

        if (self->server_ && self->sessionId_ != 0) {
          self->server_->unregisterSession(self->sessionId_);
        }
      });
}

void ClientSession::doRead() {
  auto self = shared_from_this();
  ws_.async_read(
      buffer_,
      [self](beast::error_code ec, std::size_t bytes) {
        self->onRead(ec, bytes);
      });
}

void ClientSession::onRead(beast::error_code ec, std::size_t) {
  if (ec == websocket::error::closed) {
    open_ = false;
    if (server_ && sessionId_ != 0) {
      server_->unregisterSession(sessionId_);
    }
    return;
  }
  if (ec) {
    std::cerr << "[ClientSession] read: " << ec.message() << "\n";
    return;
  }

  // Extract text frame
  const std::string text = beast::buffers_to_string(buffer_.data());
  buffer_.consume(buffer_.size());

  // TODO: Wire into ExecutionContext / MarketDispatcher here.
  // For now, just echo back the same payload to prove the loop works.
  sendText(text);

  // Continue reading
  doRead();
}

void ClientSession::onWrite(beast::error_code ec, std::size_t) {
  if (ec) {
    std::cerr << "[ClientSession] write: " << ec.message() << "\n";
    return;
  }
  // nothing else to do
}

} // namespace gma
