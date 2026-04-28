#include "gma/market/MarketConnector.hpp"

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/MarketTA.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/feed/IFeedAdapter.hpp"
#include "gma/feed/ItchAdapter.hpp"
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObProvider.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/server/FeedServer.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"
#include "gma/ws/WsFeedClient.hpp"

namespace gma::market {

MarketConnector::MarketConnector()  = default;
MarketConnector::~MarketConnector() = default;

void MarketConnector::installDefaults() {
  Dispatcher::setDefaultComputerFactory(
    [](const util::Config& cfg) {
      std::vector<std::unique_ptr<engine::IEventComputer>> out;
      out.push_back(std::make_unique<MarketTickComputer>(cfg));
      return out;
    });
}

void MarketConnector::registerWith(engine::EngineRegistries& reg) {
  using util::logger;
  using util::LogLevel;

  if (!reg.cfg || !reg.shutdown || !reg.io || !reg.dispatcher) {
    throw std::runtime_error("MarketConnector: incomplete EngineRegistries");
  }
  const util::Config& cfg = *reg.cfg;

  // Ensure the computer factory hook is installed — safe no-op if the caller
  // already did it, so consumers don't have to remember two steps.
  installDefaults();

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

  reg.shutdown->registerStep("ob-provider-clear", 50, [] {
    AtomicProviderRegistry::clear();
  });

  // ---- TCP FeedServer (market message schema) ----
  const unsigned short feedPort = static_cast<unsigned short>(cfg.feedPort);
  _feedServer = std::make_unique<FeedServer>(*reg.io, reg.dispatcher,
                                             _obManager.get(), feedPort);
  _feedServer->run();

  FeedServer* feedPtr = _feedServer.get();
  reg.shutdown->registerStep("feed-stop", 55, [feedPtr] {
    try { feedPtr->stop(); } catch (...) {}
  });

  // ---- External WS feed clients ----
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

  for (std::size_t i = 0; i < effectiveFeeds.size(); ++i) {
    const auto& fc = effectiveFeeds[i];
    if (fc.url.empty()) continue;

    try {
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
      client->start();
      _feedClients.push_back(client);

      logger().log(LogLevel::Info, "feed_client.started",
                   {{"url", fc.url}, {"adapter", fc.adapter},
                    {"index", std::to_string(i)}});
    } catch (const std::exception& ex) {
      logger().log(LogLevel::Error, "feed_client.start_failed",
                   {{"err", ex.what()}, {"url", fc.url},
                    {"index", std::to_string(i)}});
    }
  }
  for (auto& fc : _feedClients) {
    auto weak = std::weak_ptr<ws::WsFeedClient>(fc);
    reg.shutdown->registerStep("feed-ws-stop", 56, [weak] {
      if (auto s = weak.lock()) {
        try { s->stop(); } catch (...) {}
      }
    });
  }
}

} // namespace gma::market
