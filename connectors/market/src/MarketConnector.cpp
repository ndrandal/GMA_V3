#include "gma/market/MarketConnector.hpp"

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <atomic>
#include <sstream>
#include <string>
#include <string_view>

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/MarketTA.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/engine/ConfigNamespaceRegistry.hpp"
#include "gma/engine/EngineRegistries.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/engine/IngressRegistry.hpp"
#include "gma/market/MarketIngress.hpp"
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

namespace {

// Parse a comma-separated list into a vector<string>, trimming whitespace and
// dropping empty tokens.
std::vector<std::string> splitCSV(std::string_view value) {
  std::vector<std::string> out;
  std::string s(value);
  std::istringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    auto b = tok.find_first_not_of(" \t");
    auto e = tok.find_last_not_of(" \t");
    if (b == std::string::npos) continue;
    out.emplace_back(tok.substr(b, e - b + 1));
  }
  return out;
}

// Apply a single key=value observation to a MarketFieldMap, where `tail` is
// the part of the config key after the namespace prefix (e.g. "priceFields"
// or "name"). Returns true if the key was recognized.
bool applyMarketSourceKey(MarketFieldMap& fm,
                          std::string_view tail,
                          std::string_view value) {
  if (tail == "name")              { fm.name = std::string(value); return true; }
  if (tail == "priceFields")       { fm.priceFields  = splitCSV(value);
                                     if (fm.priceFields.empty())
                                       fm.priceFields = {"lastPrice", "price", "last", "px"};
                                     return true; }
  if (tail == "volumeFields")      { fm.volumeFields = splitCSV(value);
                                     if (fm.volumeFields.empty())
                                       fm.volumeFields = {"volume", "vol", "qty", "size"};
                                     return true; }
  if (tail == "bidFields")         { fm.bidFields    = splitCSV(value); return true; }
  if (tail == "askFields")         { fm.askFields    = splitCSV(value); return true; }
  if (tail == "timestampField")    { fm.timestampField = std::string(value); return true; }
  if (tail == "taEnabled")         { fm.taEnabled = (value == "true" || value == "1" || value == "yes");
                                     return true; }
  return false;
}

} // namespace

MarketConnector::MarketConnector()  = default;
MarketConnector::~MarketConnector() = default;

void MarketConnector::registerWith(engine::EngineRegistries& reg) {
  if (!reg.cfg || !reg.io || !reg.dispatcher || !reg.computers || !reg.configNs) {
    throw std::runtime_error("MarketConnector: incomplete EngineRegistries");
  }
  const util::Config& cfg = *reg.cfg;

  // ---- MarketFieldMap (populated by the configNs reader during dispatchPendingKeys) ----
  _fieldMap = std::make_shared<MarketFieldMap>();

  // Canonical namespace: keys live under "market.source.*". The reader
  // receives the tail (everything after "market.") and routes the
  // "source.<x>" subset into the field map.
  auto fieldMap = _fieldMap;
  reg.configNs->registerNamespace("market",
    [fieldMap](std::string_view tail, std::string_view value) -> bool {
      if (tail.rfind("source.", 0) != 0) return false;
      return applyMarketSourceKey(*fieldMap, tail.substr(7), value);
    });

  // Legacy "source.*" namespace: one-release deprecation alias. Logged once
  // per process; the warning surfaces in tests via Logger.
  static std::atomic<bool> deprecationWarned{false};
  reg.configNs->registerNamespace("source",
    [fieldMap](std::string_view tail, std::string_view value) -> bool {
      bool expected = false;
      if (deprecationWarned.compare_exchange_strong(expected, true)) {
        util::logger().log(util::LogLevel::Warn,
          "config.deprecated_prefix",
          {{"prefix", "source"}, {"replacement", "market.source"},
           {"window", "one release"}});
      }
      return applyMarketSourceKey(*fieldMap, tail, value);
    });

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

  // Register the two market ingress factories. Engine driver instantiates
  // each entry of cfg.ingress[] whose kind matches; factories close over
  // the connector-owned OrderBookManager so feed handlers can write into
  // it directly. Per-entry params (port, url, adapter, symbols) come from
  // the parsed ingress.N.* sub-keys.
  reg.ingress->registerIngress("market.feedserver",
    [obManager](engine::EngineRegistries& r,
                const engine::IngressParams& params) -> std::unique_ptr<engine::IIngressSource> {
      unsigned short port = 9001;
      auto pit = params.find("port");
      if (pit != params.end()) {
        try { port = static_cast<unsigned short>(std::stoi(pit->second)); }
        catch (...) {}
      }
      auto fs = std::make_unique<FeedServer>(*r.io, r.dispatcher, obManager.get(), port);
      return std::make_unique<FeedServerIngress>(std::move(fs));
    });

  reg.ingress->registerIngress("market.wsclient",
    [obManager](engine::EngineRegistries& r,
                const engine::IngressParams& params) -> std::unique_ptr<engine::IIngressSource> {
      std::string url;
      std::string adapter = "itch";
      auto uit = params.find("url");      if (uit != params.end()) url = uit->second;
      auto ait = params.find("adapter");  if (ait != params.end()) adapter = ait->second;
      std::vector<std::string> symbols;
      auto sit = params.find("symbols");
      if (sit != params.end()) {
        std::stringstream ss(sit->second);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
          auto b = tok.find_first_not_of(" \t");
          auto e = tok.find_last_not_of(" \t");
          if (b != std::string::npos) symbols.emplace_back(tok.substr(b, e - b + 1));
        }
      }
      if (symbols.empty()) symbols = {"*"};

      std::unique_ptr<feed::IFeedAdapter> ad;
      if (adapter == "itch") ad = std::make_unique<feed::ItchAdapter>();
      else                   ad = std::make_unique<feed::ItchAdapter>();  // default ITCH

      auto client = std::make_shared<ws::WsFeedClient>(
          *r.io, r.dispatcher, obManager.get(),
          url, std::move(ad), symbols);
      return std::make_unique<WsFeedClientIngress>(std::move(client));
    });

  // Register the "tick" computer factory through the engine registry. Each
  // Dispatcher's onTick lazily instantiates one MarketTickComputer per type
  // using the dispatcher's own cfg + this connector's configured field map.
  // The factory captures _fieldMap by shared_ptr so updates from the
  // configNs reader (which run during dispatchPendingKeys, AFTER
  // registerWith) reach future computer instances.
  auto fieldMapShared = _fieldMap;
  reg.computers->registerFactory("tick",
    [fieldMapShared](const util::Config& dispatcherCfg) {
      return std::make_unique<MarketTickComputer>(dispatcherCfg, *fieldMapShared);
    });

  // ENC-31: FeedServer + WsFeedClient construction is no longer the
  // connector's job. The engine driver (main.cpp) instantiates them via
  // the registered ingress factories above using cfg.ingress[] entries
  // (which include the legacy synthesis path for feedPort/feedUrl/
  // feeds.N.*). Connector just registers the factories and ob.* atomic
  // provider; nothing else to do here.
}

void MarketConnector::start() {
  // ENC-31: ingress sources are engine-driven now. The connector has no
  // long-lived process to start beyond the ob.* AtomicProviderRegistry
  // entry that registerWith already published.
}

void MarketConnector::stop() noexcept {
  // Clear the ob.* AtomicProvider namespace so subsequent listener
  // resolutions return nullopt. Order: connectors-stop (priority 30)
  // runs first, then ingress-stop (35) tears down FeedServer +
  // WsFeedClient — same temporal sequence as the pre-ENC-31 per-step
  // priorities (ob-provider-clear=50, feed-stop=55, feed-ws-stop=56).
  try { AtomicProviderRegistry::clear(); } catch (...) {}
}

} // namespace gma::market
