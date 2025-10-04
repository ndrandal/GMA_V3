// src/server/FeedServer.cpp
#include "gma/server/FeedServer.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <rapidjson/document.h>
#include <iostream>

#include "gma/server/WebSocketServer.hpp" // if needed by implementation
#include <boost/asio/io_context.hpp>


using tcp = boost::asio::ip::tcp;
namespace websocket = boost::beast::websocket;

namespace server {

//
// --- FeedSession -------------------------------------------------------------
//
class FeedSession : public std::enable_shared_from_this<FeedSession> {
public:
    FeedSession(tcp::socket socket, gma::MarketDispatcher& dispatcher)
      : _ws(std::move(socket))
      , _dispatcher(dispatcher)
    {}

    void run() {
        _ws.async_accept(
            boost::beast::bind_front_handler(&FeedSession::onAccept,
                                             shared_from_this()));
    }

private:
    void onAccept(boost::system::error_code ec) {
        if (ec) {
            std::cerr << "FeedSession handshake error: " << ec.message() << "\n";
            return;
        }
        doRead();
    }

    void doRead() {
        _ws.async_read(_buffer,
            boost::beast::bind_front_handler(&FeedSession::onRead,
                                             shared_from_this()));
    }

    void onRead(boost::system::error_code ec, std::size_t bytes) {
        if (ec == websocket::error::closed) return;
        if (ec) {
            std::cerr << "FeedSession read error: " << ec.message() << "\n";
            return;
        }

        // Parse incoming JSON
        auto text = boost::beast::buffers_to_string(_buffer.data());
        rapidjson::Document doc;
        doc.Parse(text.c_str());
        if (!doc.HasParseError() && doc.HasMember("symbol")) {
            gma::SymbolTick tick{
              doc["symbol"].GetString(),
              std::make_shared<rapidjson::Document>(std::move(doc))
            };
            _dispatcher.onTick(tick);
        }

        _buffer.consume(bytes);
        doRead();
    }

    websocket::stream<tcp::socket> _ws;
    boost::beast::flat_buffer      _buffer;
    gma::MarketDispatcher&         _dispatcher;
};

//
// --- FeedServer --------------------------------------------------------------
//
FeedServer::FeedServer(boost::asio::io_context& ioc,
                       gma::MarketDispatcher& dispatcher,
                       unsigned short port)
  : _acceptor(ioc, tcp::endpoint{tcp::v4(), port})
  , _ioc(ioc)
  , _dispatcher(dispatcher)
{}

void FeedServer::run() {
    doAccept();
}

void FeedServer::doAccept() {
    _acceptor.async_accept(
        boost::asio::make_strand(_ioc.get_executor()),
        [this](boost::system::error_code ec, tcp::socket socket) {
            onAccept(ec, std::move(socket));
        }
    );
}

void FeedServer::onAccept(boost::system::error_code ec,
                          tcp::socket socket) {
    if (ec) {
        std::cerr << "FeedServer accept error: " << ec.message() << "\n";
    } else {
        // Spawn session to handle this feed connection
        std::make_shared<FeedSession>(std::move(socket), _dispatcher)->run();
    }
    // Accept the next connection
    doAccept();
}

} // namespace server
