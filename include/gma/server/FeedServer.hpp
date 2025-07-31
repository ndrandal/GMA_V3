// src/server/FeedServer.hpp
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <memory>
#include "gma/MarketDispatcher.hpp"

namespace server {

class FeedSession;
  
/// Listens for incoming WebSocket connections and spawns FeedSession for each.
class FeedServer {
public:
    /// @param ioc         your io_context
    /// @param dispatcher  your MarketDispatcher instance
    /// @param port        TCP port to listen on (e.g. 9001)
    FeedServer(boost::asio::io_context& ioc,
               gma::MarketDispatcher& dispatcher,
               unsigned short port);

    /// Start accepting connections (non-blocking).
    void run();

private:
    void doAccept();
    void onAccept(boost::system::error_code ec,
                  boost::asio::ip::tcp::socket socket);

    boost::asio::ip::tcp::acceptor _acceptor;
    boost::asio::io_context&      _ioc;
    gma::MarketDispatcher&        _dispatcher;
};

} // namespace server
