#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "gma/engine/IConnector.hpp"
#include "gma/market/MarketFieldMap.hpp"

namespace gma {
class OrderBookManager;
namespace ob       { class Provider; class FunctionalSnapshotSource; }
namespace ws       { class WsFeedClient; }
class FeedServer;
} // namespace gma

namespace gma::market {

// The market connector owns everything market-specific: order book engine,
// TA tick computer (registered with EventComputerRegistry), "ob.*" atomic
// provider namespace, TCP FeedServer, and external WsFeedClients driven by
// ItchAdapter.
//
// Lifecycle (driven by the composition root):
//   MarketConnector connector;
//   connector.registerWith(regs);   // construct + register; nothing running
//   connector.start();              // run the feed server + WS clients
//   ...
//   connector.stop();               // reverse-order teardown (idempotent)
class MarketConnector final : public engine::IConnector {
public:
  MarketConnector();
  ~MarketConnector() override;

  std::string_view name() const override { return "market"; }

  // Construct OrderBookManager, wire its provider into AtomicProviderRegistry
  // under "ob.*", construct (do NOT run) the TCP FeedServer on cfg.feedPort,
  // construct (do NOT start) configured WsFeedClients, and register the
  // "tick" event-computer factory.
  void registerWith(engine::EngineRegistries& reg) override;

  // Run the feed server + WS feed clients. Failures on individual feed
  // clients are logged and do not abort the rest.
  void start() override;

  // Reverse-order teardown. Idempotent: a second call is a no-op.
  void stop() noexcept override;

private:
  std::shared_ptr<OrderBookManager>                  _obManager;
  std::shared_ptr<ob::FunctionalSnapshotSource>      _snapSource;
  std::shared_ptr<ob::Provider>                      _obProvider;
  std::unique_ptr<FeedServer>                        _feedServer;
  std::vector<std::shared_ptr<ws::WsFeedClient>>     _feedClients;

  // Populated by ConfigNamespaceRegistry callbacks during
  // Config::dispatchPendingKeys(); shared with MarketTickComputer
  // instances via the EventComputerRegistry factory closure.
  std::shared_ptr<MarketFieldMap>                    _fieldMap;
};

} // namespace gma::market
