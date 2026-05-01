#include "gma/market/MarketIngress.hpp"

#include "gma/feed/IFeedAdapter.hpp"
#include "gma/server/FeedServer.hpp"
#include "gma/ws/WsFeedClient.hpp"

namespace gma::market {

// ---------- FeedServerIngress ----------

FeedServerIngress::FeedServerIngress(std::unique_ptr<FeedServer> server)
  : server_(std::move(server)) {}

FeedServerIngress::~FeedServerIngress() = default;

void FeedServerIngress::start() {
  if (started_ || !server_) return;
  server_->run();
  started_ = true;
}

void FeedServerIngress::stop() noexcept {
  if (!started_ || !server_) return;
  try { server_->stop(); } catch (...) {}
  started_ = false;
}

// ---------- WsFeedClientIngress ----------

WsFeedClientIngress::WsFeedClientIngress(std::shared_ptr<ws::WsFeedClient> client)
  : client_(std::move(client)) {}

WsFeedClientIngress::~WsFeedClientIngress() = default;

void WsFeedClientIngress::start() {
  if (started_ || !client_) return;
  client_->start();
  started_ = true;
}

void WsFeedClientIngress::stop() noexcept {
  if (!started_ || !client_) return;
  try { client_->stop(); } catch (...) {}
  started_ = false;
}

} // namespace gma::market
