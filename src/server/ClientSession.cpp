#include "gma/server/ClientSession.hpp"
#include "gma/ClientConnection.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/Logger.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <iostream>
#include <string>

namespace gma {

ClientSession::ClientSession(tcp::socket&& socket,
                             ExecutionContext* ctx,
                             MarketDispatcher* dispatcher)
  : _ws(std::move(socket)),
    _ctx(ctx),
    _dispatcher(dispatcher),
    _connection(std::make_unique<ClientConnection>(
        ctx, dispatcher, &_registry,
        [this](int key, const SymbolValue& val) { sendResult(key, val); }))
{}

void ClientSession::start() {
  _ws.async_accept(
    beast::bind_front_handler(&ClientSession::onAccept, shared_from_this()));
}

void ClientSession::onAccept(beast::error_code ec) {
  if (ec) {
    std::cerr << "[Client] Accept failed: " << ec.message() << "\n";
    return;
  }
  doRead();
}

void ClientSession::doRead() {
  _ws.async_read(_buffer,
    beast::bind_front_handler(&ClientSession::onRead, shared_from_this()));
}

void ClientSession::onRead(beast::error_code ec, std::size_t) {
  if (ec == websocket::error::closed || ec == boost::asio::error::operation_aborted) {
    std::cerr << "[Client] Disconnected\n";
    _registry.shutdownAll();
    return;
  }

  if (ec) {
    std::cerr << "[Client] Read error: " << ec.message() << "\n";
    _registry.shutdownAll();
    return;
  }

  std::string msg = beast::buffers_to_string(_buffer.data());
  _buffer.consume(_buffer.size());

  try {
    _connection->onMessage(msg);
  } catch (const std::exception& e) {
    rapidjson::Document errDoc;
    errDoc.SetObject();
    auto& allocator = errDoc.GetAllocator();

    std::string errStr = "Failed to parse or process message: ";
    errStr += e.what();

    errDoc.AddMember("type", "error", allocator);
    errDoc.AddMember("error",
      rapidjson::Value(errStr.c_str(), static_cast<rapidjson::SizeType>(errStr.length()), allocator),
      allocator);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    errDoc.Accept(writer);

    _ws.text(true);
    _ws.async_write(net::buffer(buffer.GetString(), buffer.GetSize()),
                    [](beast::error_code, std::size_t) {});
  }

  doRead();
}

void ClientSession::sendResult(int key, const SymbolValue& val) {
  rapidjson::Document doc;
  doc.SetObject();
  auto& alloc = doc.GetAllocator();

  bool isError = val.symbol == "*" && std::holds_alternative<std::string>(val.value);
  const char* typeStr = isError ? "error" : "response";
  doc.AddMember(rapidjson::Value("type", alloc),
                rapidjson::Value(typeStr, alloc),
                alloc);

  doc.AddMember("key", key, alloc);

  if (isError) {
    const std::string& errMsg = std::get<std::string>(val.value);
    doc.AddMember("error",
      rapidjson::Value(errMsg.c_str(), static_cast<rapidjson::SizeType>(errMsg.length()), alloc),
      alloc);
  } else {
    const std::string& symbolStr = val.symbol;
    doc.AddMember("symbol",
      rapidjson::Value(symbolStr.c_str(), static_cast<rapidjson::SizeType>(symbolStr.length()), alloc),
      alloc);

    if (std::holds_alternative<int>(val.value)) {
      doc.AddMember("value", std::get<int>(val.value), alloc);
    } else if (std::holds_alternative<double>(val.value)) {
      doc.AddMember("value", std::get<double>(val.value), alloc);
    } else if (std::holds_alternative<std::string>(val.value)) {
      const std::string& strVal = std::get<std::string>(val.value);
      doc.AddMember("value",
        rapidjson::Value(strVal.c_str(), static_cast<rapidjson::SizeType>(strVal.length()), alloc),
        alloc);
    } else {
      doc.AddMember("value",
        rapidjson::Value("(unsupported type)", alloc),
        alloc);
    }
  }

  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);

  _ws.text(true);
  _ws.async_write(net::buffer(buffer.GetString(), buffer.GetSize()),
                  [](beast::error_code ec, std::size_t) {
                    if (ec) std::cerr << "[Client] Send failed: " << ec.message() << "\n";
                  });
}

void ClientSession::close() noexcept {
    beast::error_code ec;

    // 1) Send a normal CLOSE frame
    _ws.close(websocket::close_code::normal, ec);
    if (ec) {
        Logger::warn(std::string("ClientSession: error closing websocket: ") + ec.message());
    }

    // 2) Then shut down the underlying TCP socket
    _ws.next_layer().shutdown(tcp::socket::shutdown_both, ec);
    if (ec && ec != beast::errc::not_connected) {
        Logger::warn(std::string("ClientSession: error shutting down socket: ") + ec.message());
    }
}

} // namespace gma
