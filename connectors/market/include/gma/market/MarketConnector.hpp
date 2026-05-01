#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "gma/engine/IConnector.hpp"

namespace gma {
class OrderBookManager;
namespace ob       { class Provider; class FunctionalSnapshotSource; }
namespace ws       { class WsFeedClient; }
class FeedServer;
} // namespace gma

namespace gma::market {

// The market connector owns everything market-specific: order book engine,
// TA tick computer (via DefaultComputerFactory hook), "ob.*" atomic provider
// namespace, TCP FeedServer, and external WsFeedClients driven by ItchAdapter.
//
// Usage in main():
//   MarketConnector::installDefaults();          // install TA computer hook
//   // ... construct engine pieces (pool/store/dispatcher/ioc) ...
//   MarketConnector connector;
//   EngineRegistries regs{ &cfg, pool, store, dispatcher, &shutdown, &ioc };
//   connector.registerWith(regs);                // everything else
class MarketConnector final : public engine::IConnector {
public:
  // Install the Dispatcher DefaultComputerFactory hook so every
  // dispatcher constructed after this call gets its own MarketTickComputer.
  // Safe to call multiple times (idempotent replace).
  static void installDefaults();

  MarketConnector();
  ~MarketConnector() override;

  std::string_view name() const override { return "market"; }

  // Full market boot: construct OrderBookManager, wire its provider into the
  // AtomicProviderRegistry under "ob.*", start the TCP FeedServer on
  // cfg.feedPort, spin up configured WsFeedClients, and register all
  // shutdown steps on reg.shutdown. Returns with the servers running inside
  // reg.io (which the caller drives via io.run()).
  void registerWith(engine::EngineRegistries& reg) override;
  void start() override;
  void stop() noexcept override;

private:
  std::shared_ptr<OrderBookManager>                  _obManager;
  std::shared_ptr<ob::FunctionalSnapshotSource>      _snapSource;
  std::shared_ptr<ob::Provider>                      _obProvider;
  std::unique_ptr<FeedServer>                        _feedServer;
  std::vector<std::shared_ptr<ws::WsFeedClient>>     _feedClients;
};

} // namespace gma::market
