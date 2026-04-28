#include "gma/ws/WsFeedClient.hpp"

#include "gma/feed/IFeedAdapter.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/Event.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <boost/beast/core/buffers_to_string.hpp>

#include <algorithm>

namespace gma::ws {

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace net       = boost::asio;
using tcp           = net::ip::tcp;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
WsFeedClient::WsFeedClient(net::io_context& ioc,
                           Dispatcher* dispatcher,
                           OrderBookManager* obManager,
                           const std::string& url,
                           std::unique_ptr<feed::IFeedAdapter> adapter,
                           std::vector<std::string> symbols)
  : ioc_(ioc)
  , strand_(net::make_strand(ioc))
  , resolver_(strand_)
  , reconnectTimer_(strand_)
#ifdef GMA_HAS_SSL
  , sslCtx_(boost::asio::ssl::context::tlsv12_client)
#endif
  , dispatcher_(dispatcher)
  , obManager_(obManager)
  , adapter_(std::move(adapter))
  , subscribeSymbols_(std::move(symbols))
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

  // Send subscribe message — adapter controls the wire format
  auto msg = std::make_shared<std::string>(
      adapter_->subscribeMessage(subscribeSymbols_));

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

  int capped = std::min(reconnectAttempts_, 30);
  int delaySec = std::min(1 << capped, MAX_RECONNECT_DELAY_SEC);
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
// Message handling — delegate to adapter, dispatch resulting events.
// Routing: TickEvent → Dispatcher (TA computation + raw-field listeners).
//          Ob* events → OrderBookManager (book mutations + trades).
// ---------------------------------------------------------------------------
void WsFeedClient::handleMessage(const std::string& text) {
  auto events = adapter_->translate(text);
  for (auto& evt : events) {
    dispatchEvent(evt);
  }
}

void WsFeedClient::dispatchEvent(feed::FeedEvent& evt) {
  std::visit([this](auto& e) {
    using T = std::decay_t<decltype(e)>;

    if constexpr (std::is_same_v<T, feed::TickEvent>) {
      if (dispatcher_) {
        Event tick;
        tick.symbol  = std::move(e.symbol);
        tick.payload = std::move(e.payload);
        dispatcher_->onTick(tick);
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObAddEvent>) {
      if (obManager_) {
        obManager_->onAdd(e.symbol, e.orderId, e.side, e.price, e.size, e.priority);
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObUpdateEvent>) {
      if (obManager_) {
        obManager_->onUpdate(e.symbol, e.orderId, FeedScope{}, e.newPrice, e.newSize);
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObDeleteEvent>) {
      if (obManager_) {
        obManager_->onDelete(e.symbol, e.orderId, FeedScope{});
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObTradeEvent>) {
      if (obManager_) {
        obManager_->onTrade(e.symbol, e.price, e.size, e.aggressor);
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObTickSizeEvent>) {
      if (obManager_) {
        obManager_->setTickSize(e.symbol, e.tickSize);
      }
    }
    else if constexpr (std::is_same_v<T, feed::ObResetEvent>) {
      if (obManager_) {
        obManager_->onReset(e.symbol, e.epoch);
      }
    }
  }, evt);
}

} // namespace gma::ws
