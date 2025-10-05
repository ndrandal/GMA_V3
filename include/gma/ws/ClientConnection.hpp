#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <functional>
#include <memory>
#include <deque>
#include <string>
#include <string_view>

namespace gma { namespace ws {

class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
public:
  using IoContext     = boost::asio::io_context;
  using Tcp           = boost::asio::ip::tcp;
  using WsStream      = boost::beast::websocket::stream<boost::beast::tcp_stream>;
  using ErrorCode     = boost::beast::error_code;

  using OnOpen    = std::function<void()>;
  using OnClose   = std::function<void()>;
  using OnMessage = std::function<void(const std::string&)>;
  using OnError   = std::function<void(const ErrorCode&, std::string_view where)>;

  // #### Ctors — multiple overloads to match common call sites ####
  ClientConnection(IoContext& ioc,
                   std::string host,
                   unsigned short port,
                   std::string target,
                   OnMessage onMessage = {},
                   OnOpen onOpen = {},
                   OnError onError = {});

  ClientConnection(IoContext& ioc,
                   std::string host,
                   std::string service_or_port, // "80" or "8080"
                   std::string target,
                   OnMessage onMessage = {},
                   OnOpen onOpen = {},
                   OnError onError = {});

  // Convenience factories
  static std::shared_ptr<ClientConnection>
  create(IoContext& ioc,
         std::string host,
         unsigned short port,
         std::string target,
         OnMessage onMessage = {},
         OnOpen onOpen = {},
         OnError onError = {})
  {
    return std::make_shared<ClientConnection>(ioc, std::move(host), port,
                                              std::move(target),
                                              std::move(onMessage),
                                              std::move(onOpen),
                                              std::move(onError));
  }

  static std::shared_ptr<ClientConnection>
  create(IoContext& ioc,
         std::string host,
         std::string service_or_port,
         std::string target,
         OnMessage onMessage = {},
         OnOpen onOpen = {},
         OnError onError = {})
  {
    return std::make_shared<ClientConnection>(ioc, std::move(host),
                                              std::move(service_or_port),
                                              std::move(target),
                                              std::move(onMessage),
                                              std::move(onOpen),
                                              std::move(onError));
  }

  // Lifecycle
  void connect();
  void close();          // graceful close
  bool isOpen() const;

  // Sending (thread-safe; queued)
  void send(std::string text);

  // Optional callbacks
  void setOnOpen(OnOpen cb)       { onOpen_ = std::move(cb); }
  void setOnClose(OnClose cb)     { onClose_ = std::move(cb); }
  void setOnMessage(OnMessage cb) { onMessage_ = std::move(cb); }
  void setOnError(OnError cb)     { onError_ = std::move(cb); }

private:
  // Async chain
  void onResolve(ErrorCode ec, Tcp::resolver::results_type results);
  void onConnect(ErrorCode ec, Tcp::resolver::results_type::endpoint_type ep);
  void onHandshake(ErrorCode ec);

  // Read loop
  void doRead();
  void onRead(ErrorCode ec, std::size_t bytes);

  // Write queue
  void doWrite();
  void onWrite(ErrorCode ec, std::size_t bytes);

  // Helpers
  void fail(ErrorCode ec, std::string_view where);

private:
  IoContext& ioc_;
  boost::asio::strand<boost::asio::io_context::executor_type> strand_;
  Tcp::resolver resolver_;
  WsStream ws_;
  std::string host_;
  std::string service_;   // numeric port as string (e.g. "80")
  std::string target_;    // e.g. "/ws"
  boost::beast::flat_buffer buffer_;

  std::deque<std::string> outbox_;
  bool writing_{false};
  bool closing_{false};

  OnOpen onOpen_;
  OnClose onClose_; // (notified from outside if you wire it where appropriate)
  OnMessage onMessage_;
  OnError onError_;
};

}} // namespace gma::ws
