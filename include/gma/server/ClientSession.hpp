#pragma once

#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/RequestRegistry.hpp"
#include "gma/ClientConnection.hpp"

#include <boost/beast/websocket.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <memory>
#include <string>

namespace gma {

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  ClientSession(tcp::socket&& socket,
                ExecutionContext* ctx,
                MarketDispatcher* dispatcher);

  void start();
  void sendResult(int key, const SymbolValue& val);
  void close() noexcept;
private:
  websocket::stream<tcp::socket> _ws;
  beast::flat_buffer _buffer;

  ExecutionContext* _ctx;
  MarketDispatcher* _dispatcher;
  RequestRegistry _registry;

  std::unique_ptr<ClientConnection> _connection;

  void onAccept(beast::error_code ec);
  void doRead();
  void onRead(beast::error_code ec, std::size_t bytes_transferred);
};

} // namespace gma
