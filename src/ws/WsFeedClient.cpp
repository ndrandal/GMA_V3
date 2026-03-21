#include "gma/ws/WsFeedClient.hpp"

#include "gma/MarketDispatcher.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <boost/beast/core/buffers_to_string.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdlib>
#include <algorithm>
#include <charconv>

namespace gma::ws {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
WsFeedClient::WsFeedClient(net::io_context& ioc,
                           MarketDispatcher* dispatcher,
                           OrderBookManager* obManager,
                           const std::string& url,
                           std::vector<std::string> symbols)
  : ioc_(ioc)
  , strand_(net::make_strand(ioc))
  , resolver_(strand_)
  , reconnectTimer_(strand_)
  , dispatcher_(dispatcher)
  , obManager_(obManager)
  , subscribeSymbols_(std::move(symbols))
#ifdef GMA_HAS_SSL
  , sslCtx_(boost::asio::ssl::context::tlsv12_client)
#endif
{
  url_ = parseUrl(url);

#ifdef GMA_HAS_SSL
  // Use the system's default certificate store for TLS verification.
  sslCtx_.set_default_verify_paths();
  sslCtx_.set_verify_mode(boost::asio::ssl::verify_peer);
#endif
}

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------
WsFeedClient::Url WsFeedClient::parseUrl(const std::string& url) {
  Url u;
  std::string rest;

  if (url.substr(0, 6) == "wss://") {
    u.tls = true;
    rest = url.substr(6);
  } else if (url.substr(0, 5) == "ws://") {
    u.tls = false;
    rest = url.substr(5);
  } else {
    throw std::runtime_error("WsFeedClient: URL must start with ws:// or wss://");
  }

  // Split host[:port]/target
  auto slashPos = rest.find('/');
  std::string hostPort = (slashPos != std::string::npos) ? rest.substr(0, slashPos) : rest;
  u.target = (slashPos != std::string::npos) ? rest.substr(slashPos) : "/";

  auto colonPos = hostPort.rfind(':');
  if (colonPos != std::string::npos) {
    u.host = hostPort.substr(0, colonPos);
    u.port = hostPort.substr(colonPos + 1);
  } else {
    u.host = hostPort;
    u.port = u.tls ? "443" : "80";
  }

  return u;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void WsFeedClient::start() {
  stopping_.store(false);
  reconnectAttempts_ = 0;

  gma::util::logger().log(gma::util::LogLevel::Info,
    "WsFeedClient.start",
    {{"host", url_.host}, {"port", url_.port},
     {"target", url_.target}, {"tls", url_.tls ? "true" : "false"}});

  net::post(strand_, [self = shared_from_this()]{ self->doResolve(); });
}

void WsFeedClient::stop() {
  stopping_.store(true);

  net::post(strand_, [self = shared_from_this()]{
    self->reconnectTimer_.cancel();

    beast::error_code ec;
    if (self->ws_ && self->ws_->is_open()) {
      self->ws_->close(websocket::close_code::normal, ec);
    }
#ifdef GMA_HAS_SSL
    if (self->wss_ && self->wss_->is_open()) {
      self->wss_->close(websocket::close_code::normal, ec);
    }
#endif
  });
}

void WsFeedClient::resetStreams() {
  ws_.reset();
#ifdef GMA_HAS_SSL
  wss_.reset();
#endif
  buffer_.clear();
}

// ---------------------------------------------------------------------------
// Connection chain
// ---------------------------------------------------------------------------
void WsFeedClient::doResolve() {
  if (stopping_.load()) return;

  resetStreams();

  resolver_.async_resolve(url_.host, url_.port,
    beast::bind_front_handler(&WsFeedClient::onResolve, shared_from_this()));
}

void WsFeedClient::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
  if (ec) return fail(ec, "resolve");
  if (stopping_.load()) return;

  // Create the appropriate stream
  if (url_.tls) {
#ifdef GMA_HAS_SSL
    wss_ = std::make_unique<SslWs>(strand_, sslCtx_);
    // SNI hostname
    if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(), url_.host.c_str())) {
      return fail(beast::error_code(static_cast<int>(::ERR_get_error()),
                                     net::error::get_ssl_category()), "sni");
    }
    beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*wss_).async_connect(results,
      beast::bind_front_handler(&WsFeedClient::onTcpConnect, shared_from_this()));
#else
    gma::util::logger().log(gma::util::LogLevel::Error,
      "WsFeedClient: wss:// requires GMA_HAS_SSL (install libssl-dev and rebuild)");
    return;
#endif
  } else {
    ws_ = std::make_unique<PlainWs>(strand_);
    beast::get_lowest_layer(*ws_).expires_after(std::chrono::seconds(30));
    beast::get_lowest_layer(*ws_).async_connect(results,
      beast::bind_front_handler(&WsFeedClient::onTcpConnect, shared_from_this()));
  }
}

void WsFeedClient::onTcpConnect(beast::error_code ec,
                                tcp::resolver::results_type::endpoint_type) {
  if (ec) return fail(ec, "tcp_connect");
  if (stopping_.load()) return;

  if (url_.tls) {
#ifdef GMA_HAS_SSL
    beast::get_lowest_layer(*wss_).expires_after(std::chrono::seconds(30));
    wss_->next_layer().async_handshake(boost::asio::ssl::stream_base::client,
      beast::bind_front_handler(&WsFeedClient::onSslHandshake, shared_from_this()));
#endif
  } else {
    // Skip SSL, go straight to WS handshake
    onSslHandshake({});
  }
}

void WsFeedClient::onSslHandshake(beast::error_code ec) {
  if (ec) return fail(ec, "ssl_handshake");
  if (stopping_.load()) return;

  // Set WS options and perform handshake
  auto hostHeader = url_.host + ":" + url_.port;

  if (url_.tls) {
#ifdef GMA_HAS_SSL
    beast::get_lowest_layer(*wss_).expires_never();
    wss_->set_option(websocket::stream_base::decorator(
      [](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "gma-feed-client");
      }));
    wss_->async_handshake(hostHeader, url_.target,
      beast::bind_front_handler(&WsFeedClient::onWsHandshake, shared_from_this()));
#endif
  } else {
    beast::get_lowest_layer(*ws_).expires_never();
    ws_->set_option(websocket::stream_base::decorator(
      [](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "gma-feed-client");
      }));
    ws_->async_handshake(hostHeader, url_.target,
      beast::bind_front_handler(&WsFeedClient::onWsHandshake, shared_from_this()));
  }
}

void WsFeedClient::onWsHandshake(beast::error_code ec) {
  if (ec) return fail(ec, "ws_handshake");
  if (stopping_.load()) return;

  reconnectAttempts_ = 0;

  gma::util::logger().log(gma::util::LogLevel::Info,
    "WsFeedClient.connected",
    {{"host", url_.host}, {"target", url_.target}});
  GMA_METRIC_HIT("feed_ws.connected");

  // Send subscribe message
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("action"); w.String("subscribe");
  w.Key("symbols");
  w.StartArray();
  for (const auto& s : subscribeSymbols_) w.String(s.c_str());
  w.EndArray();
  w.EndObject();

  auto msg = std::make_shared<std::string>(sb.GetString());

  auto writeCb = [self = shared_from_this(), msg](beast::error_code ec2, std::size_t) {
    if (ec2) return self->fail(ec2, "subscribe_write");
    self->doRead();
  };

  if (url_.tls) {
#ifdef GMA_HAS_SSL
    wss_->text(true);
    wss_->async_write(net::buffer(*msg), std::move(writeCb));
#endif
  } else {
    ws_->text(true);
    ws_->async_write(net::buffer(*msg), std::move(writeCb));
  }
}

// ---------------------------------------------------------------------------
// Read loop
// ---------------------------------------------------------------------------
void WsFeedClient::doRead() {
  if (stopping_.load()) return;

  auto readCb = beast::bind_front_handler(&WsFeedClient::onRead, shared_from_this());

  if (url_.tls) {
#ifdef GMA_HAS_SSL
    wss_->async_read(buffer_, std::move(readCb));
#endif
  } else {
    ws_->async_read(buffer_, std::move(readCb));
  }
}

void WsFeedClient::onRead(beast::error_code ec, std::size_t) {
  if (ec) {
    if (ec == websocket::error::closed) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
        "WsFeedClient: feed connection closed");
    } else {
      fail(ec, "read");
    }
    if (!stopping_.load()) scheduleReconnect();
    return;
  }

  auto text = beast::buffers_to_string(buffer_.cdata());
  buffer_.consume(buffer_.size());

  GMA_METRIC_HIT("feed_ws.msg_in");

  try {
    handleMessage(text);
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Warn,
      "WsFeedClient.handleMessage exception",
      {{"err", ex.what()}});
  }

  doRead();
}

// ---------------------------------------------------------------------------
// Reconnect
// ---------------------------------------------------------------------------
void WsFeedClient::scheduleReconnect() {
  if (stopping_.load()) return;

  int delaySec = std::min(1 << reconnectAttempts_, MAX_RECONNECT_DELAY_SEC);
  ++reconnectAttempts_;

  gma::util::logger().log(gma::util::LogLevel::Info,
    "WsFeedClient.reconnect",
    {{"delaySec", std::to_string(delaySec)},
     {"attempt", std::to_string(reconnectAttempts_)}});

  reconnectTimer_.expires_after(std::chrono::seconds(delaySec));
  reconnectTimer_.async_wait([self = shared_from_this()](beast::error_code ec) {
    if (ec || self->stopping_.load()) return;
    self->doResolve();
  });
}

void WsFeedClient::fail(beast::error_code ec, const char* where) {
  gma::util::logger().log(gma::util::LogLevel::Error,
    "WsFeedClient failure",
    {{"where", where}, {"err", ec.message()}});
  GMA_METRIC_HIT("feed_ws.error");
}

// ---------------------------------------------------------------------------
// Message routing
// ---------------------------------------------------------------------------
void WsFeedClient::handleMessage(const std::string& text) {
  rapidjson::Document doc;
  doc.Parse(text.c_str());
  if (doc.HasParseError() || !doc.IsObject()) return;

  if (!doc.HasMember("type") || !doc["type"].IsString()) return;

  const std::string type = doc["type"].GetString();

  if      (type == "trade")                routeTrade(doc);
  else if (type == "add_order")            routeAddOrder(doc);
  else if (type == "add_order_mpid")       routeAddOrder(doc);
  else if (type == "order_executed")        routeOrderExecuted(doc);
  else if (type == "order_cancel")          routeOrderCancel(doc);
  else if (type == "order_delete")          routeOrderDelete(doc);
  else if (type == "order_replace")         routeOrderReplace(doc);
  else if (type == "stock_directory")       routeStockDirectory(doc);
  else if (type == "system_event")          routeSystemEvent(doc);
  else if (type == "stock_trading_action")  { /* TODO: halt/resume */ }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
std::string WsFeedClient::resolveSymbol(int stockLocate) const {
  auto it = locateToSymbol_.find(stockLocate);
  return (it != locateToSymbol_.end()) ? it->second : std::string{};
}

double WsFeedClient::parsePrice(const rapidjson::Value& v) {
  // Feed sends price as string "185.2500" or number
  if (v.IsString()) return std::strtod(v.GetString(), nullptr);
  if (v.IsNumber()) return v.GetDouble();
  return 0.0;
}

// ---------------------------------------------------------------------------
// stock_directory → setTickSize + register symbol
// ---------------------------------------------------------------------------
void WsFeedClient::routeStockDirectory(const rapidjson::Value& doc) {
  if (!doc.HasMember("stockLocate") || !doc.HasMember("stock")) return;
  if (!doc["stock"].IsString()) return;

  int locate = doc["stockLocate"].GetInt();
  std::string stock = doc["stock"].GetString();

  // Trim trailing spaces (ITCH pads to 8 chars)
  while (!stock.empty() && stock.back() == ' ') stock.pop_back();

  locateToSymbol_[locate] = stock;

  // All symbols in this feed use $0.01 tick size
  if (obManager_) {
    obManager_->setTickSize(stock, 0.01);
  }

  GMA_METRIC_HIT("feed_ws.stock_directory");

  gma::util::logger().log(gma::util::LogLevel::Info,
    "WsFeedClient.stockDirectory",
    {{"locate", std::to_string(locate)}, {"stock", stock}});
}

// ---------------------------------------------------------------------------
// add_order / add_order_mpid → OB add + track order state
// ---------------------------------------------------------------------------
void WsFeedClient::routeAddOrder(const rapidjson::Value& doc) {
  if (!doc.HasMember("stock") || !doc["stock"].IsString()) return;
  if (!doc.HasMember("orderRef") || !doc.HasMember("side") ||
      !doc.HasMember("shares") || !doc.HasMember("price")) return;

  std::string stock = doc["stock"].GetString();
  while (!stock.empty() && stock.back() == ' ') stock.pop_back();

  uint64_t orderRef = doc["orderRef"].GetUint64();
  uint64_t shares   = doc["shares"].GetUint64();
  double price      = parsePrice(doc["price"]);

  const char* sideStr = doc["side"].GetString();
  Side side = (sideStr[0] == 'B') ? Side::Bid : Side::Ask;

  // Track order state for executed/cancel messages
  orders_[orderRef] = {stock, shares, side, price};

  if (obManager_) {
    obManager_->onAdd(stock, orderRef, side, price, shares, 0);
  }

  GMA_METRIC_HIT("feed_ws.add_order");
}

// ---------------------------------------------------------------------------
// order_executed → reduce size or delete, emit trade tick
// ---------------------------------------------------------------------------
void WsFeedClient::routeOrderExecuted(const rapidjson::Value& doc) {
  if (!doc.HasMember("orderRef") || !doc.HasMember("shares")) return;

  uint64_t orderRef = doc["orderRef"].GetUint64();
  uint64_t execShares = doc["shares"].GetUint64();

  auto it = orders_.find(orderRef);
  if (it == orders_.end()) return;

  auto& os = it->second;
  const std::string& symbol = os.symbol;

  if (obManager_) {
    if (execShares >= os.remainingShares) {
      // Fully filled — delete the order
      obManager_->onDelete(symbol, orderRef, FeedScope{});
      // Also record as a trade
      Aggressor aggr = (os.side == Side::Bid) ? Aggressor::Buy : Aggressor::Sell;
      obManager_->onTrade(symbol, os.price, execShares, aggr);
    } else {
      // Partial fill — reduce size
      os.remainingShares -= execShares;
      obManager_->onUpdate(symbol, orderRef, FeedScope{},
                           std::nullopt, os.remainingShares);
      Aggressor aggr = (os.side == Side::Bid) ? Aggressor::Buy : Aggressor::Sell;
      obManager_->onTrade(symbol, os.price, execShares, aggr);
    }
  }

  // Emit a tick for the trade so TA computations fire
  if (dispatcher_) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& a = payload->GetAllocator();
    payload->AddMember("symbol",
      rapidjson::Value(symbol.c_str(), a), a);
    payload->AddMember("lastPrice", os.price, a);
    payload->AddMember("volume", static_cast<double>(execShares), a);

    SymbolTick tick;
    tick.symbol = symbol;
    tick.payload = std::move(payload);
    dispatcher_->onTick(tick);
  }

  if (execShares >= it->second.remainingShares) {
    orders_.erase(it);
  }

  GMA_METRIC_HIT("feed_ws.order_executed");
}

// ---------------------------------------------------------------------------
// order_cancel → partial cancel (reduce remaining shares)
// ---------------------------------------------------------------------------
void WsFeedClient::routeOrderCancel(const rapidjson::Value& doc) {
  if (!doc.HasMember("orderRef") || !doc.HasMember("shares")) return;

  uint64_t orderRef = doc["orderRef"].GetUint64();
  uint64_t cancelShares = doc["shares"].GetUint64();

  auto it = orders_.find(orderRef);
  if (it == orders_.end()) return;

  auto& os = it->second;

  if (obManager_) {
    if (cancelShares >= os.remainingShares) {
      obManager_->onDelete(os.symbol, orderRef, FeedScope{});
      orders_.erase(it);
    } else {
      os.remainingShares -= cancelShares;
      obManager_->onUpdate(os.symbol, orderRef, FeedScope{},
                           std::nullopt, os.remainingShares);
    }
  }

  GMA_METRIC_HIT("feed_ws.order_cancel");
}

// ---------------------------------------------------------------------------
// order_delete → full removal
// ---------------------------------------------------------------------------
void WsFeedClient::routeOrderDelete(const rapidjson::Value& doc) {
  if (!doc.HasMember("orderRef")) return;

  uint64_t orderRef = doc["orderRef"].GetUint64();

  auto it = orders_.find(orderRef);
  if (it == orders_.end()) return;

  if (obManager_) {
    obManager_->onDelete(it->second.symbol, orderRef, FeedScope{});
  }

  orders_.erase(it);
  GMA_METRIC_HIT("feed_ws.order_delete");
}

// ---------------------------------------------------------------------------
// order_replace → delete old + add new
// ---------------------------------------------------------------------------
void WsFeedClient::routeOrderReplace(const rapidjson::Value& doc) {
  if (!doc.HasMember("origOrderRef") || !doc.HasMember("orderRef") ||
      !doc.HasMember("shares") || !doc.HasMember("price")) return;

  uint64_t origRef = doc["origOrderRef"].GetUint64();
  uint64_t newRef  = doc["orderRef"].GetUint64();
  uint64_t shares  = doc["shares"].GetUint64();
  double price     = parsePrice(doc["price"]);

  auto it = orders_.find(origRef);
  if (it == orders_.end()) return;

  std::string symbol = it->second.symbol;
  Side side = it->second.side;

  // Delete old
  if (obManager_) {
    obManager_->onDelete(symbol, origRef, FeedScope{});
  }
  orders_.erase(it);

  // Add new
  orders_[newRef] = {symbol, shares, side, price};
  if (obManager_) {
    obManager_->onAdd(symbol, newRef, side, price, shares, 0);
  }

  GMA_METRIC_HIT("feed_ws.order_replace");
}

// ---------------------------------------------------------------------------
// trade → OB trade + dispatcher tick
// ---------------------------------------------------------------------------
void WsFeedClient::routeTrade(const rapidjson::Value& doc) {
  if (!doc.HasMember("stock") || !doc["stock"].IsString() ||
      !doc.HasMember("price") || !doc.HasMember("shares")) return;

  std::string stock = doc["stock"].GetString();
  while (!stock.empty() && stock.back() == ' ') stock.pop_back();

  double price    = parsePrice(doc["price"]);
  uint64_t shares = doc["shares"].GetUint64();

  Aggressor aggr = Aggressor::Unknown;
  if (doc.HasMember("side") && doc["side"].IsString()) {
    const char* s = doc["side"].GetString();
    if (s[0] == 'B') aggr = Aggressor::Buy;
    else if (s[0] == 'S') aggr = Aggressor::Sell;
  }

  if (obManager_) {
    obManager_->onTrade(stock, price, shares, aggr);
  }

  // Emit tick to dispatcher for TA computation
  if (dispatcher_) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& a = payload->GetAllocator();
    payload->AddMember("symbol",
      rapidjson::Value(stock.c_str(), a), a);
    payload->AddMember("lastPrice", price, a);
    payload->AddMember("volume", static_cast<double>(shares), a);

    SymbolTick tick;
    tick.symbol = stock;
    tick.payload = std::move(payload);
    dispatcher_->onTick(tick);
  }

  GMA_METRIC_HIT("feed_ws.trade");
}

// ---------------------------------------------------------------------------
// system_event — log only
// ---------------------------------------------------------------------------
void WsFeedClient::routeSystemEvent(const rapidjson::Value& doc) {
  std::string code = "?";
  if (doc.HasMember("eventCode") && doc["eventCode"].IsString())
    code = doc["eventCode"].GetString();

  gma::util::logger().log(gma::util::LogLevel::Info,
    "WsFeedClient.systemEvent",
    {{"eventCode", code}});
}

} // namespace gma::ws
