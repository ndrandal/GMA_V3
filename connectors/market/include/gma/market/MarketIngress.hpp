#pragma once

// Thin IIngressSource adapters that wrap the existing FeedServer + WsFeedClient
// types. Lets the engine drive their lifecycle uniformly through
// IngressRegistry without changing those classes' public surfaces.

#include <memory>
#include <string>
#include <vector>

#include "gma/engine/IngressRegistry.hpp"

namespace gma {
class FeedServer;
class OrderBookManager;
namespace ws { class WsFeedClient; }
namespace feed { class IFeedAdapter; }
} // namespace gma

namespace gma::market {

// Wraps a FeedServer instance: start() runs the server (acceptor opens +
// async_accept loop); stop() noexcept shuts it down. Constructed by the
// market.feedserver factory at engine ingress-driver invocation time.
class FeedServerIngress final : public engine::IIngressSource {
public:
  FeedServerIngress(std::unique_ptr<FeedServer> server);
  ~FeedServerIngress() override;

  void start() override;
  void stop() noexcept override;

private:
  std::unique_ptr<FeedServer> server_;
  bool started_{false};
};

// Wraps a WsFeedClient instance.
class WsFeedClientIngress final : public engine::IIngressSource {
public:
  WsFeedClientIngress(std::shared_ptr<ws::WsFeedClient> client);
  ~WsFeedClientIngress() override;

  void start() override;
  void stop() noexcept override;

private:
  std::shared_ptr<ws::WsFeedClient> client_;
  bool started_{false};
};

} // namespace gma::market
