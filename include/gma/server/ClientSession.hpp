#pragma once

#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/RequestRegistry.hpp"
#include "gma/ClientConnection.hpp"
#include "gma/SymbolValue.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace gma {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = boost::asio::ip::tcp;

/// One WebSocket client connection.
class ClientSession : public std::enable_shared_from_this<ClientSession> {
public:
  using SendTextFn = std::function<void(const std::string&)>;

  // Factory to keep construction in one place
  static std::shared_ptr<ClientSession> create(
      tcp::socket&& socket,
      ExecutionContext* ctx,
      MarketDispatcher* dispatcher) {
    return std::shared_ptr<ClientSession>(
      new ClientSession(std::move(socket), ctx, dispatcher));
  }

  // Begin handshake and start read loop
  void run();

  // Idempotent stop/close
  void stop() noexcept;

private:
  ClientSession(tcp::socket&& socket,
                ExecutionContext* ctx,
                MarketDispatcher* dispatcher);

  // Handlers
  void onAccept(beast::error_code ec);
  void doRead();
  void onRead(beast::error_code ec, std::size_t bytes);
  void doWrite();
  void onWrite(beast::error_code ec, std::size_t bytes);
  void safeClose(beast::error_code ec) noexcept;

  // Bridge from app layer (ClientConnection) to outbound WS text
  void sendUpdateFromApp(int key, const SymbolValue& val);

  // Serialize a SymbolValue payload to JSON text
  static std::string serializeUpdateJson(int key, const SymbolValue& val);

private:
  std::shared_ptr<websocket::stream<beast::tcp_stream>> ws_;
  beast::flat_buffer  buffer_;

  // Write queue (single-threaded via the ws_ executor)
  std::deque<std::string> writeQueue_;
  bool writeInFlight_{false};
  std::atomic<bool> closed_{false};

  // Application plumbing
  ExecutionContext*  ctx_;
  MarketDispatcher*  dispatcher_;
  RequestRegistry    registry_;
  std::unique_ptr<ClientConnection> connection_;
};

} // namespace gma
