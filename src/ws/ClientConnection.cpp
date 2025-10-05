#include "gma/ws/ClientConnection.hpp"
#include <boost/beast/version.hpp>
#include <chrono>
#include <iostream>

namespace gma { namespace ws {

using namespace std::chrono_literals;

ClientConnection::ClientConnection(IoContext& ioc,
                                   std::string host,
                                   unsigned short port,
                                   std::string target,
                                   OnMessage onMessage,
                                   OnOpen onOpen,
                                   OnError onError)
  : ioc_(ioc)
  , strand_(boost::asio::make_strand(ioc))
  , resolver_(strand_)
  , ws_(strand_)
  , host_(std::move(host))
  , service_(std::to_string(port))
  , target_(std::move(target))
  , onOpen_(std::move(onOpen))
  , onMessage_(std::move(onMessage))
  , onError_(std::move(onError))
{
}

ClientConnection::ClientConnection(IoContext& ioc,
                                   std::string host,
                                   std::string service_or_port,
                                   std::string target,
                                   OnMessage onMessage,
                                   OnOpen onOpen,
                                   OnError onError)
  : ioc_(ioc)
  , strand_(boost::asio::make_strand(ioc))
  , resolver_(strand_)
  , ws_(strand_)
  , host_(std::move(host))
  , service_(std::move(service_or_port))
  , target_(std::move(target))
  , onOpen_(std::move(onOpen))
  , onMessage_(std::move(onMessage))
  , onError_(std::move(onError))
{
}

bool ClientConnection::isOpen() const {
  return ws_.is_open();
}

void ClientConnection::connect() {
  closing_ = false;

  // Resolve host:service
  resolver_.async_resolve(host_, service_,
    boost::asio::bind_executor(
      strand_,
      [self = shared_from_this()](ErrorCode ec, Tcp::resolver::results_type results) {
        self->onResolve(ec, std::move(results));
      }
    )
  );
}

void ClientConnection::onResolve(ErrorCode ec, Tcp::resolver::results_type results) {
  if (ec) return fail(ec, "resolve");

  // Set a reasonable timeout on the tcp layer
  boost::beast::get_lowest_layer(ws_).expires_after(30s);

  // Connect to the first endpoint that works
  boost::beast::get_lowest_layer(ws_).async_connect(
    results,
    boost::asio::bind_executor(
      strand_,
      [self = shared_from_this()](ErrorCode ec2, Tcp::resolver::results_type::endpoint_type ep) {
        self->onConnect(ec2, ep);
      }
    )
  );
}

void ClientConnection::onConnect(ErrorCode ec, Tcp::resolver::results_type::endpoint_type) {
  if (ec) return fail(ec, "connect");

  // Turn off the timeout on the tcp layer; the websocket stream has its own timeouts.
  boost::beast::get_lowest_layer(ws_).expires_never();

  // Decorate request (User-Agent / Host will be set by handshake)
  ws_.set_option(boost::beast::websocket::stream_base::decorator(
    [](boost::beast::websocket::request_type& req){
      req.set(boost::beast::http::field::user_agent, "gma-ws-client");
    }));

  // We expect to exchange text frames by default
  ws_.text(true);

  // Perform the websocket handshake
  ws_.async_handshake(host_, target_,
    boost::asio::bind_executor(
      strand_,
      [self = shared_from_this()](ErrorCode ec2){
        self->onHandshake(ec2);
      }
    )
  );
}

void ClientConnection::onHandshake(ErrorCode ec) {
  if (ec) return fail(ec, "handshake");

  // Connected
  if (onOpen_) onOpen_();

  // Start reading
  doRead();

  // If anything was enqueued before open, kick writes
  if (!outbox_.empty() && !writing_) {
    writing_ = true;
    doWrite();
  }
}

void ClientConnection::doRead() {
  ws_.async_read(
    buffer_,
    boost::asio::bind_executor(
      strand_,
      [self = shared_from_this()](ErrorCode ec, std::size_t bytes){
        self->onRead(ec, bytes);
      }
    )
  );
}

void ClientConnection::onRead(ErrorCode ec, std::size_t) {
  if (ec) {
    if (ec == boost::beast::websocket::error::closed) {
      if (onClose_) onClose_();
      return;
    }
    return fail(ec, "read");
  }

  // Deliver message
  if (onMessage_) {
    auto text = boost::beast::buffers_to_string(buffer_.cdata());
    onMessage_(text);
  }
  buffer_.consume(buffer_.size());

  // Continue reading
  doRead();
}

void ClientConnection::send(std::string text) {
  // All socket ops must run on the strand
  boost::asio::post(
    strand_,
    [self = shared_from_this(), msg = std::move(text)]() mutable {
      self->outbox_.emplace_back(std::move(msg));
      if (!self->writing_) {
        self->writing_ = true;
        self->doWrite();
      }
    }
  );
}

void ClientConnection::doWrite() {
  if (outbox_.empty()) {
    writing_ = false;
    return;
  }

  // Ensure text frame mode
  ws_.text(true);

  ws_.async_write(
    boost::asio::buffer(outbox_.front()),
    boost::asio::bind_executor(
      strand_,
      [self = shared_from_this()](ErrorCode ec, std::size_t bytes){
        self->onWrite(ec, bytes);
      }
    )
  );
}

void ClientConnection::onWrite(ErrorCode ec, std::size_t) {
  if (ec) return fail(ec, "write");

  outbox_.pop_front();
  if (!outbox_.empty()) {
    doWrite();
  } else {
    writing_ = false;
  }
}

void ClientConnection::close() {
  boost::asio::post(
    strand_,
    [self = shared_from_this()](){
      if (self->closing_ || !self->ws_.is_open())
        return;

      self->closing_ = true;
      self->ws_.async_close(
        boost::beast::websocket::close_code::normal,
        boost::asio::bind_executor(
          self->strand_,
          [self](ErrorCode ec){
            if (ec) self->fail(ec, "close");
            if (self->onClose_) self->onClose_();
          }
        )
      );
    }
  );
}

void ClientConnection::fail(ErrorCode ec, std::string_view where) {
  if (onError_) onError_(ec, where);
  else std::cerr << "[ClientConnection] " << where << ": " << ec.message() << "\n";
}

}} // namespace gma::ws
