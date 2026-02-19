#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>

#include <boost/asio/ip/tcp.hpp>

namespace gma {

class MarketDispatcher;   // fwd-declare to keep header light
class OrderBookManager;   // fwd-declare for OB routing

/// Minimal TCP feed server that accepts producer connections and forwards
/// incoming messages to a MarketDispatcher. The actual deserialization is
/// done inside the session implementation (cpp), not exposed here.
class FeedServer {
public:
  using tcp = boost::asio::ip::tcp;

  FeedServer(boost::asio::io_context& ioc,
             MarketDispatcher* dispatcher,
             unsigned short port);

  FeedServer(boost::asio::io_context& ioc,
             MarketDispatcher* dispatcher,
             OrderBookManager* obManager,
             unsigned short port);

  FeedServer(const FeedServer&) = delete;
  FeedServer& operator=(const FeedServer&) = delete;

  void run();
  void stop();

private:
  class FeedSession; // pimpl session

  void doAccept();
  void onAccept(boost::system::error_code ec, tcp::socket socket);

private:
  static constexpr std::size_t MAX_FEED_SESSIONS = 64;

  boost::asio::io_context& ioc_;
  tcp::acceptor            acceptor_;
  std::atomic<bool>        accepting_{false};

  MarketDispatcher*        dispatcher_; // not owned
  OrderBookManager*        obManager_{nullptr}; // not owned

  std::mutex                                  mu_;
  std::unordered_set<std::shared_ptr<FeedSession>> sessions_;
};

} // namespace gma
