#include "gma/server/FeedServer.hpp"

#include <boost/asio/strand.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>

#include <deque>
#include <utility>

// Project headers (adjust paths if needed)
#include "gma/MarketDispatcher.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <rapidjson/document.h>

namespace gma {

using tcp = boost::asio::ip::tcp;

// ---------------------- FeedSession ----------------------
class FeedServer::FeedSession : public std::enable_shared_from_this<FeedSession> {
public:
  FeedSession(tcp::socket socket,
              MarketDispatcher* dispatcher,
              OrderBookManager* obManager,
              FeedServer* owner)
    : socket_(std::move(socket))
    , dispatcher_(dispatcher)
    , obManager_(obManager)
    , owner_(owner)
  {}

  void start() { doRead(); }

  void close() {
    boost::system::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    // Remove ourselves from the owner's session set.
    if (owner_) {
      std::lock_guard<std::mutex> lk(owner_->mu_);
      owner_->sessions_.erase(shared_from_this());
    }
  }

private:
  void doRead() {
    auto self = shared_from_this();
    socket_.async_read_some(
      boost::asio::buffer(buf_),
      [self](boost::system::error_code ec, std::size_t n) {
        self->onRead(ec, n);
      });
  }

  void onRead(boost::system::error_code ec, std::size_t n) {
    if (ec) {
      // Session ends on error
      close();
      return;
    }

    // Very simple framing: treat incoming as newline-delimited text messages
    pending_.insert(pending_.end(), buf_.data(), buf_.data() + n);
    std::size_t start = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i] == '\n') {
        const std::string line(pending_.data() + start, pending_.data() + i);
        handleLine(line);
        start = i + 1;
      }
    }
    if (start > 0) {
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(start));
    }

    doRead(); // continue reading
  }

  static constexpr std::size_t MAX_LINE_SIZE = 64 * 1024; // 64 KB

  void handleLine(const std::string& line) {
    GMA_METRIC_HIT("feed.line_in");

    // Guard against oversized lines (DoS prevention).
    if (line.size() > MAX_LINE_SIZE) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.line.too_large",
                              {{"size", std::to_string(line.size())}});
      return;
    }

    try {
      rapidjson::Document doc;
      doc.Parse(line.c_str());
      if (doc.HasParseError() || !doc.IsObject()) {
        GMA_METRIC_HIT("feed.tick_bad");
        gma::util::logger().log(gma::util::LogLevel::Warn,
                                "feed.line.bad_json",
                                {{"line", line.substr(0, 200)}});
        return;
      }

      // Route by "type" field
      if (doc.HasMember("type") && doc["type"].IsString()) {
        const std::string type = doc["type"].GetString();
        if (type == "ob") {
          handleObMessage(doc);
          return;
        }
        if (type == "control") {
          handleControlMessage(doc);
          return;
        }
        // Unknown type — fall through to tick path
      }

      // Default: market tick path
      handleTickMessage(doc, line);

    } catch (const std::exception& ex) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "feed.handleLine exception",
                              {{"err", ex.what()}});
    } catch (...) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "feed.handleLine unknown exception");
    }
  }

  void handleTickMessage(rapidjson::Document& doc, const std::string& line) {
    if (!dispatcher_) return;

    if (!doc.HasMember("symbol") || !doc["symbol"].IsString()) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.line.missing_symbol",
                              {{"line", line.substr(0, 200)}});
      return;
    }

    gma::SymbolTick t;
    t.symbol = doc["symbol"].GetString();

    // Move doc onto heap for shared ownership in the tick
    auto shared = std::make_shared<rapidjson::Document>(std::move(doc));
    t.payload = std::move(shared);

    GMA_METRIC_HIT("feed.tick_ok");
    GMA_METRIC_HIT("dispatch.tick");
    dispatcher_->onTick(t);
  }

  void handleObMessage(const rapidjson::Document& doc) {
    if (!obManager_) {
      GMA_METRIC_HIT("feed.ob_no_manager");
      return;
    }

    if (!doc.HasMember("symbol") || !doc["symbol"].IsString()) {
      GMA_METRIC_HIT("feed.ob_bad");
      return;
    }
    if (!doc.HasMember("action") || !doc["action"].IsString()) {
      GMA_METRIC_HIT("feed.ob_bad");
      return;
    }

    const std::string symbol = doc["symbol"].GetString();
    const std::string action = doc["action"].GetString();

    if (action == "ticksize") {
      if (!doc.HasMember("tickSize") || !doc["tickSize"].IsNumber()) {
        GMA_METRIC_HIT("feed.ob_bad");
        return;
      }
      obManager_->setTickSize(symbol, doc["tickSize"].GetDouble());
      GMA_METRIC_HIT("feed.ob_ticksize");

    } else if (action == "add") {
      if (!doc.HasMember("id") || !doc["id"].IsUint64()) { GMA_METRIC_HIT("feed.ob_bad"); return; }
      if (!doc.HasMember("side") || !doc["side"].IsString()) { GMA_METRIC_HIT("feed.ob_bad"); return; }
      if (!doc.HasMember("price") || !doc["price"].IsNumber()) { GMA_METRIC_HIT("feed.ob_bad"); return; }
      if (!doc.HasMember("size") || !doc["size"].IsUint64()) { GMA_METRIC_HIT("feed.ob_bad"); return; }

      const std::string sideStr = doc["side"].GetString();
      Side side = (sideStr == "ask") ? Side::Ask : Side::Bid;
      uint64_t priority = doc.HasMember("priority") && doc["priority"].IsUint64()
                            ? doc["priority"].GetUint64() : 0;

      obManager_->onAdd(symbol, doc["id"].GetUint64(), side,
                        doc["price"].GetDouble(), doc["size"].GetUint64(), priority);
      GMA_METRIC_HIT("feed.ob_add");

    } else if (action == "update") {
      if (!doc.HasMember("id") || !doc["id"].IsUint64()) { GMA_METRIC_HIT("feed.ob_bad"); return; }

      std::optional<double> newPrice;
      std::optional<uint64_t> newSize;
      if (doc.HasMember("price") && doc["price"].IsNumber())
        newPrice = doc["price"].GetDouble();
      if (doc.HasMember("size") && doc["size"].IsUint64())
        newSize = doc["size"].GetUint64();

      obManager_->onUpdate(symbol, doc["id"].GetUint64(), FeedScope{}, newPrice, newSize);
      GMA_METRIC_HIT("feed.ob_update");

    } else if (action == "delete") {
      if (!doc.HasMember("id") || !doc["id"].IsUint64()) { GMA_METRIC_HIT("feed.ob_bad"); return; }

      obManager_->onDelete(symbol, doc["id"].GetUint64(), FeedScope{});
      GMA_METRIC_HIT("feed.ob_delete");

    } else if (action == "trade") {
      if (!doc.HasMember("price") || !doc["price"].IsNumber()) { GMA_METRIC_HIT("feed.ob_bad"); return; }
      if (!doc.HasMember("size") || !doc["size"].IsUint64()) { GMA_METRIC_HIT("feed.ob_bad"); return; }

      Aggressor aggr = Aggressor::Unknown;
      if (doc.HasMember("aggressor") && doc["aggressor"].IsString()) {
        const std::string a = doc["aggressor"].GetString();
        if (a == "buy") aggr = Aggressor::Buy;
        else if (a == "sell") aggr = Aggressor::Sell;
      }

      obManager_->onTrade(symbol, doc["price"].GetDouble(), doc["size"].GetUint64(), aggr);
      GMA_METRIC_HIT("feed.ob_trade");

    } else {
      GMA_METRIC_HIT("feed.ob_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.ob.unknown_action",
                              {{"action", action}});
    }
  }

  void handleControlMessage(const rapidjson::Document& doc) {
    if (!doc.HasMember("action") || !doc["action"].IsString()) {
      GMA_METRIC_HIT("feed.control_bad");
      return;
    }

    const std::string action = doc["action"].GetString();

    if (action == "reset") {
      if (!doc.HasMember("symbol") || !doc["symbol"].IsString()) {
        GMA_METRIC_HIT("feed.control_bad");
        return;
      }
      uint32_t epoch = 0;
      if (doc.HasMember("epoch") && doc["epoch"].IsUint())
        epoch = doc["epoch"].GetUint();

      if (obManager_) {
        obManager_->onReset(doc["symbol"].GetString(), epoch);
        GMA_METRIC_HIT("feed.control_reset");
      }
    } else {
      GMA_METRIC_HIT("feed.control_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.control.unknown_action",
                              {{"action", action}});
    }
  }

private:
  tcp::socket        socket_;
  MarketDispatcher*  dispatcher_{nullptr};  // not owned
  OrderBookManager*  obManager_{nullptr};   // not owned
  FeedServer*        owner_{nullptr};       // not owned

  std::array<char, 8 * 1024> buf_{};
  std::vector<char>          pending_;
};

// ---------------------- FeedServer ----------------------

FeedServer::FeedServer(boost::asio::io_context& ioc,
                       MarketDispatcher* dispatcher,
                       unsigned short port)
  : FeedServer(ioc, dispatcher, nullptr, port)
{}

FeedServer::FeedServer(boost::asio::io_context& ioc,
                       MarketDispatcher* dispatcher,
                       OrderBookManager* obManager,
                       unsigned short port)
  : ioc_(ioc),
    acceptor_(ioc),
    dispatcher_(dispatcher),
    obManager_(obManager)
{
  boost::system::error_code ec;
  tcp::endpoint ep{tcp::v4(), port};

  acceptor_.open(ep.protocol(), ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.bind(ep, ec);
  if (ec) throw boost::system::system_error(ec);

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) throw boost::system::system_error(ec);
}

void FeedServer::run() {
  bool expected = false;
  if (!accepting_.compare_exchange_strong(expected, true)) return;
  doAccept();
}

void FeedServer::stop() {
  accepting_.store(false);

  boost::system::error_code ec;
  acceptor_.cancel(ec);

  std::unordered_set<std::shared_ptr<FeedSession>> copy;
  {
    std::lock_guard<std::mutex> lk(mu_);
    copy = sessions_;
    sessions_.clear();
  }
  for (auto& s : copy) {
    s->close();
  }
}

void FeedServer::doAccept() {
  if (!accepting_.load()) return;

  acceptor_.async_accept(
    boost::asio::make_strand(ioc_),
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void FeedServer::onAccept(boost::system::error_code ec, tcp::socket socket) {
  if (!accepting_.load()) return;

  if (!ec) {
    auto sp = std::make_shared<FeedSession>(std::move(socket), dispatcher_, obManager_, this);
    {
      std::lock_guard<std::mutex> lk(mu_);
      sessions_.insert(sp);
    }
    sp->start();
  }

  doAccept();
}

} // namespace gma
