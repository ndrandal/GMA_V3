#include "gma/server/ClientSession.hpp"
#include <iostream>

namespace gma {

ClientSession::ClientSession(boost::asio::io_context& ioc)
  : ioc_(ioc), ws_(ioc) {}

void ClientSession::start() {
  // Set text mode (replacement for the erroneous async_text)
  ws_.text(textMode_);
  doRead();
}

void ClientSession::sendText(const std::string& s) {
  ws_.text(true); // ensure text frame
  ws_.async_write(boost::asio::buffer(s),
    [self = shared_from_this()](auto ec, std::size_t n){
      self->onWrite(ec, n);
    });
}

void ClientSession::close() {
  boost::beast::error_code ec;
  ws_.close(boost::beast::websocket::close_code::normal, ec);
  if (ec) std::cerr << "ClientSession::close: " << ec.message() << "\n";
}

void ClientSession::doRead() {
  ws_.async_read(buffer_,
    [self = shared_from_this()](auto ec, std::size_t n){
      self->onRead(ec, n);
    });
}

void ClientSession::onRead(boost::beast::error_code ec, std::size_t) {
  if (ec) {
    if (ec != boost::beast::websocket::error::closed)
      std::cerr << "read: " << ec.message() << "\n";
    return;
  }
  buffer_.consume(buffer_.size());
  doRead();
}

void ClientSession::onWrite(boost::beast::error_code ec, std::size_t) {
  if (ec) std::cerr << "write: " << ec.message() << "\n";
}

} // namespace gma
