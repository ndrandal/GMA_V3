// File: src/main.cpp
#include <csignal>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <chrono>
#include <iostream>

// -------- Core (your repo) --------
#include "gma/AtomicStore.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/server/FeedServer.hpp"

// -------- Utilities --------
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/ExecutionContext.hpp"

// -------- TA --------
#include "gma/ta/AtomicNames.hpp"
#include "gma/AtomicFunctions.hpp"

// -------- OB --------
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObProvider.hpp"
#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObKeysCatalog.hpp"
#include "gma/book/OrderBookManager.hpp"

// ---------------------------
// Globals
// ---------------------------
static gma::rt::ShutdownCoordinator* g_shutdown = nullptr;

static void handleSignal(int) {
  if (g_shutdown) g_shutdown->stop();
}

static unsigned short parsePort(const char* str, unsigned short fallback) {
  try {
    unsigned long p = std::stoul(str);
    if (p == 0 || p > 65535) return fallback;
    return static_cast<unsigned short>(p);
  } catch (...) {
    return fallback;
  }
}

// ---------------------------
// main
// ---------------------------
int main(int argc, char* argv[]) {
  using namespace gma::util;

  // Shutdown coordinator — declared early so it outlives servers.
  gma::rt::ShutdownCoordinator shutdown;
  g_shutdown = &shutdown;

  // 0) Signals -> graceful stop
  std::signal(SIGINT,  handleSignal);
  std::signal(SIGTERM, handleSignal);

  // 1) Config
  //    argv[1] = wsPort (optional)
  //    argv[2] = configFilePath (optional)
  //    argv[3] = feedPort (optional)
  Config cfg;

  // Load config file first (if provided) so its port values are available as defaults
  if (argc > 2) {
    if (!cfg.loadFromFile(argv[2])) {
      std::cerr << "[config] warning: failed to load file: " << argv[2] << "\n";
    }
  }

  // CLI args override config file values
  unsigned short wsPort   = static_cast<unsigned short>(cfg.wsPort);
  unsigned short feedPort = static_cast<unsigned short>(cfg.feedPort);

  if (argc > 1) {
    wsPort = parsePort(argv[1], wsPort);
  }
  if (argc > 3) {
    feedPort = parsePort(argv[3], feedPort);
  }

  // 2) Logger
  logger().log(
    LogLevel::Info,
    "boot",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", std::to_string(feedPort)}}
  );

  // 3) Register built-in atomic functions (mean, sum, min, max, etc.)
  gma::registerBuiltinFunctions();

  // 4) Thread pool (global) — use config threadPoolSize if set
  unsigned poolSize = cfg.threadPoolSize > 0
      ? static_cast<unsigned>(cfg.threadPoolSize)
      : std::thread::hardware_concurrency();
  if (poolSize == 0) poolSize = 4;
  gma::gThreadPool = std::make_shared<gma::rt::ThreadPool>(poolSize);
  shutdown.registerStep("pool-drain",   80, []{ if (gma::gThreadPool) gma::gThreadPool->drain(); });
  shutdown.registerStep("pool-destroy", 85, []{ gma::gThreadPool.reset(); });

  // 5) Core components
  auto store      = std::make_shared<gma::AtomicStore>();
  auto dispatcher = std::make_shared<gma::MarketDispatcher>(gma::gThreadPool.get(), store.get(), cfg);

  // 6) Metrics reporter
  if (cfg.metricsEnabled) {
    gma::util::MetricRegistry::instance().startReporter(
        static_cast<unsigned>(cfg.metricsIntervalSec));
    shutdown.registerStep("metrics-stop", 10, []{
      gma::util::MetricRegistry::instance().stopReporter();
    });
  }

  // 7) Order Book system
  auto obManager = std::make_shared<gma::OrderBookManager>();

  auto snapSource = std::make_shared<gma::ob::FunctionalSnapshotSource>(
    // Capture function: produce a Snapshot from the OrderBookManager
    [obManager](const std::string& symbol,
                size_t maxLevels,
                gma::ob::Mode /*mode*/,
                std::optional<std::pair<double,double>> /*priceBand*/) -> gma::ob::Snapshot {
      gma::ob::Snapshot snap;
      auto ds = obManager->buildSnapshot(symbol, maxLevels);
      double tick = obManager->getTickSize(symbol);
      // Build bid ladder (Price is integer ticks — convert to double)
      for (const auto& [px, sz] : ds.bids) {
        double dpx = static_cast<double>(px.ticks) * tick;
        snap.bids.levels.push_back({dpx, static_cast<double>(sz),
            std::numeric_limits<double>::quiet_NaN(), dpx * static_cast<double>(sz)});
      }
      // Build ask ladder
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
    // Tick size function
    [obManager](const std::string& symbol) -> double {
      return obManager->getTickSize(symbol);
    }
  );

  auto obProvider = std::make_shared<gma::ob::Provider>(snapSource, 10, 10);

  // Register OB provider for "ob.*" keys
  gma::AtomicProviderRegistry::registerNamespace("ob",
    [obProvider](const std::string& symbol, const std::string& fullKey) -> double {
      return obProvider->get(symbol, fullKey);
    });

  shutdown.registerStep("ob-provider-clear", 50, []{ gma::AtomicProviderRegistry::clear(); });

  // 8) ASIO + servers
  boost::asio::io_context ioc;

  gma::ExecutionContext exec(store.get(), gma::gThreadPool.get());

  gma::WebSocketServer ws(ioc, &exec, dispatcher.get(), wsPort);
  ws.run();

  gma::FeedServer feed(ioc, dispatcher.get(), obManager.get(), feedPort);
  feed.run();

  // 9) Shutdown sequencing — all captured objects are still alive when shutdown runs
  shutdown.registerStep("ws-stop-accept",    5,  [&ws]{ try { ws.stopAccept(); } catch (...) {} });
  shutdown.registerStep("ws-close-sessions", 40, [&ws]{ try { ws.closeAll(); } catch (...) {} });
  shutdown.registerStep("feed-stop",         55, [&feed]{ try { feed.stop(); } catch (...) {} });
  shutdown.registerStep("asio-stop",         60, [&ioc]{ try { ioc.stop(); } catch (...) {} });

  logger().log(
    LogLevel::Info,
    "listening",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", std::to_string(feedPort)}}
  );

  // 10) Run
  try {
    ioc.run();
  } catch (const std::exception& ex) {
    logger().log(LogLevel::Error, std::string("io_context exception: ") + ex.what(), {});
  } catch (...) {
    logger().log(LogLevel::Error, "io_context exception: unknown", {});
  }

  // Ensure shutdown steps run even on natural exit
  shutdown.stop();

  g_shutdown = nullptr;
  logger().log(LogLevel::Info, "stopped", {});
  return EXIT_SUCCESS;
}
