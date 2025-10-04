#include "gma/server/ClientSession.hpp"
#include "gma/Logger.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstring>
#include <variant>

namespace gma {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
using tcp           = boost::asio::ip::tcp;

// ---- construction -----------------------------------------------------------

ClientSession::ClientSession(tcp::socket&& socket,
                             ExecutionContext* ctx,
                             MarketDispatcher* dispatcher)
  : ws_(std::make_shared<websocket::stream<beast::tcp_stream>>(std::move(socket)))
  , ctx_(ctx)
  , dispatcher_(dispatcher) {
  // Build the app-layer connection; wire its send callback into this session.
  connection_ = std::make_unique<ClientConnection>(
      ctx_, dispatcher_, &registry_,
      [this](int key, const SymbolValue& val) {
        this->sendUpdateFromApp(key, val);
      });
}

void ClientSession::run() {
  ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
  ws_->set_option(websocket::stream_base::decorator(
      [](websocket::response_type& res) {
        res.set(beast::http::field::server, "gma-ws");
      }));

  ws_->async_accept(
      beast::bind_front_handler(&ClientSession::onAccept, shared_from_this()));
}

void ClientSession::stop() noexcept {
  safeClose({});
}

// ---- accept / read / write --------------------------------------------------

void ClientSession::onAccept(beast::error_code ec) {
  if (ec) {
    safeClose(ec);
    return;
  }
  doRead();
}

void ClientSession::doRead() {
  ws_->async_read(
      buffer_,
      beast::bind_front_handler(&ClientSession::onRead, shared_from_this()));
}

void ClientSession::onRead(beast::error_code ec, std::size_t) {
  if (ec == websocket::error::closed) {
    safeClose({});
    return;
  }
  if (ec) {
    safeClose(ec);
    return;
  }

  // Convert incoming frame to string
  std::string msg = beast::buffers_to_string(buffer_.data());
  buffer_.consume(buffer_.size());

  try {
    // Let app layer parse/route the message; responses come via sendUpdateFromApp
    connection_->onMessage(msg);
  } catch (...) {
    // If the app throws, close this session
    safeClose({});
    return;
  }

  doRead();
}

void ClientSession::sendUpdateFromApp(int key, const SymbolValue& val) {
  auto text = serializeUpdateJson(key, val);
  // Marshal writes to the stream strand/executor
  boost::asio::dispatch(ws_->get_executor(),
    [self = shared_from_this(), t = std::move(text)]() mutable {
      self->writeQueue_.emplace_back(std::move(t));
      if (!self->writeInFlight_) {
        self->writeInFlight_ = true;
        self->doWrite();
      }
    });
}

void ClientSession::doWrite() {
  ws_->async_text(true);
  ws_->async_write(
      boost::asio::buffer(writeQueue_.front()),
      beast::bind_front_handler(&ClientSession::onWrite, shared_from_this()));
}

void ClientSession::onWrite(beast::error_code ec, std::size_t) {
  if (ec) {
    safeClose(ec);
    return;
  }
  writeQueue_.pop_front();
  if (!writeQueue_.empty()) {
    doWrite();
  } else {
    writeInFlight_ = false;
  }
}

void ClientSession::safeClose(beast::error_code) noexcept {
  if (closed_.exchange(true)) return;
  beast::error_code ignored;
  if (ws_ && ws_->is_open()) {
    ws_->close(websocket::close_code::normal, ignored);
  }
  // Registry will drop when this object dies; any nodes should be stoppable via RequestRegistry::shutdownAll if needed.
}

// ---- JSON serializer --------------------------------------------------------

static inline void addArgValue(rapidjson::Value& out,
                               rapidjson::Document::AllocatorType& a,
                               const gma::ArgType& v) {
  if (std::holds_alternative<bool>(v)) {
    out = rapidjson::Value(std::get<bool>(v));
  } else if (std::holds_alternative<int>(v)) {
    out = rapidjson::Value(std::get<int>(v));
  } else if (std::holds_alternative<double>(v)) {
    out = rapidjson::Value(std::get<double>(v));
  } else if (std::holds_alternative<std::string>(v)) {
    const auto& s = std::get<std::string>(v);
    out.SetString(s.c_str(), static_cast<rapidjson::SizeType>(s.size()), a);
  } else {
    // Fallback: emit string "unsupported"
    const char* s = "unsupported";
    out.SetString(s, static_cast<rapidjson::SizeType>(std::strlen(s)), a);
  }
}

std::string ClientSession::serializeUpdateJson(int key, const SymbolValue& val) {
  rapidjson::Document d;
  d.SetObject();
  auto& a = d.GetAllocator();

  d.AddMember("type", "update", a);
  d.AddMember("id", key, a);

  if (val.symbol.empty()) {
    d.AddMember("symbol", "", a);
  } else {
    rapidjson::Value sym;
    sym.SetString(val.symbol.c_str(), static_cast<rapidjson::SizeType>(val.symbol.size()), a);
    d.AddMember("symbol", sym, a);
  }

  rapidjson::Value v;
  addArgValue(v, a, val.value);
  d.AddMember("value", v, a);

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  d.Accept(w);
  return std::string(sb.GetString(), sb.GetSize());
}

} // namespace gma
