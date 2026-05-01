#include "gma/market/MarketConnector.hpp"

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/MarketTA.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/feed/IFeedAdapter.hpp"
#include "gma/feed/ItchAdapter.hpp"
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObProvider.hpp"
#include "gma/server/FeedServer.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"
#include "gma/ws/WsFeedClient.hpp"

namespace gma::market {

MarketConnector::MarketConnector()  = default;
MarketConnector::~MarketConnector() = default;

void MarketConnector::registerWith(engine::EngineRegistries& reg) {
  if (!reg.cfg || !reg.io || !reg.dispatcher || !reg.computers) {
    throw std::runtime_error("MarketConnector: incomplete EngineRegistries");
  }
  const util::Config& cfg = *reg.cfg;

  // ---- Order book engine ----
  _obManager = std::make_shared<OrderBookManager>();
  if (cfg.allowNegativePrices) {
    _obManager->setAllowNegativePrices(true);
  }

  auto obManager = _obManager;
  _snapSource = std::make_shared<ob::FunctionalSnapshotSource>(
    [obManager](const std::string& symbol,
                std::size_t maxLevels,
                ob::Mode /*mode*/,
                std::optional<std::pair<double, double>> /*priceBand*/) -> ob::Snapshot {
      ob::Snapshot snap;
      auto ds = obManager->buildSnapshot(symbol, maxLevels);
      double tick = obManager->getTickSize(symbol);
      for (const auto& [px, sz] : ds.bids) {
        double dpx = static_cast<double>(px.ticks) * tick;
        snap.bids.levels.push_back({dpx, static_cast<double>(sz),
            std::numeric_limits<double>::quiet_NaN(), dpx * static_cast<double>(sz)});
      }
      for (const auto& [px, sz] : ds.asks) {
        double dpx = static_cast<double>(px.ticks) * tick;
        snap.asks.levels.push_back({dpx, static_cast<double>(sz),
            std::numeric_limits<double>::quiet_NaN(), dpx * static_cast<double>(sz)});
      }
      snap.meta.seq = ds.seq;
      snap.meta.epoch = ds.epoch;
      snap.meta.bidLevels = snap.bids.levels.size();
      snap.meta.askLevels = snap.asks.levels.size();
      return snap;
    },
    [obManager](const std::string& symbol) -> double {
      return obManager->getTickSize(symbol);
    });

  _obProvider = std::make_shared<ob::Provider>(_snapSource, 10, 10);

  auto obProvider = _obProvider;
  AtomicProviderRegistry::registerNamespace("ob",
    [obProvider](const std::string& symbol, const std::string& fullKey) -> double {
      return obProvider->get(symbol, fullKey);
    });

  // Register the "tick" computer factory through the engine registry. Each
  // Dispatcher's onTick lazily instantiates one MarketTickComputer per type
  // using the dispatcher's own cfg, so per-Dispatcher tuning is honored.
  reg.computers->registerFactory("tick",
    [](const util::Config& dispatcherCfg) {
      return std::make_unique<MarketTickComputer>(dispatcherCfg);
    });

  // ---- TCP FeedServer (constructed, not started) ----
  const unsigned short feedPort = static_cast<unsigned short>(cfg.feedPort);
  _feedServer = std::make_unique<FeedServer>(*reg.io, reg.dispatcher,
                                             _obManager.get(), feedPort);

  // ---- External WS feed clients (constructed, not started) ----
  auto effectiveFeeds = cfg.feeds;
  if (effectiveFeeds.empty()) {
    std::string feedUrl = cfg.feedUrl;
    if (feedUrl.empty()) {
      const char* envUrl = std::getenv("GMA_FEED_URL");
      if (envUrl && envUrl[0]) feedUrl = envUrl;
    }
    if (!feedUrl.empty()) {
      util::Config::FeedConfig legacy;
      legacy.url = feedUrl;
      legacy.adapter = "itch";
      legacy.symbols = cfg.feedSymbols;
      effectiveFeeds.push_back(std::move(legacy));
    }
  }

  for (const auto& fc : effectiveFeeds) {
    if (fc.url.empty()) continue;

    std::unique_ptr<feed::IFeedAdapter> adapter;
    if (fc.adapter == "itch") {
      adapter = std::make_unique<feed::ItchAdapter>();
    } else {
      // Default to ITCH; future: "generic", "coinbase", …
      adapter = std::make_unique<feed::ItchAdapter>();
    }

    auto client = std::make_shared<ws::WsFeedClient>(
        *reg.io, reg.dispatcher, _obManager.get(),
        fc.url, std::move(adapter), fc.symbols);
    _feedClients.push_back(std::move(client));
  }
}

void MarketConnector::start() {
  using util::logger;
  using util::LogLevel;

  if (_feedServer) {
    _feedServer->run();
  }

  for (std::size_t i = 0; i < _feedClients.size(); ++i) {
    auto& client = _feedClients[i];
    if (!client) continue;
    try {
      client->start();
      logger().log(LogLevel::Info, "feed_client.started",
                   {{"index", std::to_string(i)}});
    } catch (const std::exception& ex) {
      logger().log(LogLevel::Error, "feed_client.start_failed",
                   {{"err", ex.what()}, {"index", std::to_string(i)}});
    }
  }
}

void MarketConnector::stop() noexcept {
  // Reverse of start / registerWith. Old per-step priorities preserved as
  // comments so reviewers can map the old ShutdownCoordinator order to here.
  for (auto& fc : _feedClients) {           // was priority 56 (feed-ws-stop)
    if (!fc) continue;
    try { fc->stop(); } catch (...) {}
  }
  _feedClients.clear();

  if (_feedServer) {                        // was priority 55 (feed-stop)
    try { _feedServer->stop(); } catch (...) {}
    _feedServer.reset();
  }

  try { AtomicProviderRegistry::clear(); }  // was priority 50 (ob-provider-clear)
  catch (...) {}
}

} // namespace gma::market
