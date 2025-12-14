#include "gma/server/FeedServer.hpp"

#include <boost/asio/strand.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/beast/core.hpp>

#include <deque>
#include <utility>

// Project headers (adjust paths if needed)
#include "gma/MarketDispatcher.hpp"
#include "gma/SymbolTick.hpp"
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
              FeedServer* owner)
    : socket_(std::move(socket))
    , dispatcher_(dispatcher)
    , owner_(owner)
  {}

  void start() { doRead(); }

  void close() {
    boost::system::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
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

  void handleLine(const std::string& line) {
    if (!dispatcher_) return;
    // Smoke-test framing: newline-delimited JSON objects.
    // Required: {"symbol":"AAPL", ... numeric fields ... }
    GMA_METRIC_HIT("feed.line_in");

    auto doc = std::make_shared<rapidjson::Document>();
    doc->Parse(line.c_str());
    if (doc->HasParseError() || !doc->IsObject()) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.line.bad_json",
                              { {"line", line} });
      return;
    }

    if (!doc->HasMember("symbol") || !(*doc)["symbol"].IsString()) {
      GMA_METRIC_HIT("feed.tick_bad");
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "feed.line.missing_symbol",
                              { {"line", line} });
      return;
    }

    gma::SymbolTick t;
    t.symbol  = (*doc)["symbol"].GetString();
    t.payload = std::move(doc);

    GMA_METRIC_HIT("feed.tick_ok");
    GMA_METRIC_HIT("dispatch.tick");
    dispatcher_->onTick(t);
  }

private:
  tcp::socket       socket_;
  MarketDispatcher* dispatcher_{nullptr}; // not owned
  FeedServer*       owner_{nullptr};      // not owned

  std::array<char, 8 * 1024> buf_{};
  std::vector<char>          pending_;
};

// ---------------------- FeedServer ----------------------

FeedServer::FeedServer(boost::asio::io_context& ioc,
                       MarketDispatcher* dispatcher,
                       unsigned short port)
  : ioc_(ioc),
    acceptor_(ioc),
    dispatcher_(dispatcher)
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
  if (accepting_) return;
  accepting_ = true;
  doAccept();
}

void FeedServer::stop() {
  accepting_ = false;

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
  if (!accepting_) return;

  acceptor_.async_accept(
    boost::asio::make_strand(ioc_),
    [this](boost::system::error_code ec, tcp::socket socket) {
      onAccept(ec, std::move(socket));
    });
}

void FeedServer::onAccept(boost::system::error_code ec, tcp::socket socket) {
  if (!accepting_) return;

  if (!ec) {
    auto sp = std::make_shared<FeedSession>(std::move(socket), dispatcher_, this);
    {
      std::lock_guard<std::mutex> lk(mu_);
      sessions_.insert(sp);
    }
    sp->start();
  }

  doAccept();
}

} // namespace gma
