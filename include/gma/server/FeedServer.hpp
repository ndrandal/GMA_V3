#pragma once
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

#include <boost/asio/io_context.hpp>

namespace gma {

class WebSocketServer; // fwd-declare to avoid heavy includes

class FeedServer {
public:
  using OnStart = std::function<void()>;
  using OnStop  = std::function<void()>;

  explicit FeedServer(std::shared_ptr<WebSocketServer> ws)
  : ws_(std::move(ws)) {}

  // Start the IO context + WS server on a background thread.
  void start(OnStart onStart = nullptr);

  // Stop and join.
  void stop(OnStop onStop = nullptr);

  bool running() const { return running_.load(std::memory_order_relaxed); }

private:
  std::shared_ptr<WebSocketServer> ws_;
  std::atomic<bool> running_{false};
  std::thread       thr_;
};

} // namespace gma
