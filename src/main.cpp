// File: src/main.cpp
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "gma/engine/IConnector.hpp"

// -------- Engine --------
#include "gma/AtomicStore.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/Registries.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/server/WebSocketServer.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

// -------- Connectors --------
#include "gma/market/MarketConnector.hpp"

// ---------------------------
// Globals
// ---------------------------
static std::atomic<gma::rt::ShutdownCoordinator*> g_shutdown{nullptr};

static void handleSignal(int) {
  auto* p = g_shutdown.load(std::memory_order_acquire);
  if (p) p->stop();
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
  g_shutdown.store(&shutdown, std::memory_order_release);

  // 0) Signals -> graceful stop
  std::signal(SIGINT,  handleSignal);
  std::signal(SIGTERM, handleSignal);

  // 1) Config
  //    argv[1] = wsPort (optional)
  //    argv[2] = configFilePath (optional)
  //    argv[3] = feedPort (optional)
  Config cfg;

  if (argc > 2) {
    if (!cfg.loadFromFile(argv[2])) {
      std::cerr << "[config] warning: failed to load file: " << argv[2] << "\n";
    }
  }

  if (cfg.wsPort <= 0 || cfg.wsPort > 65535) {
    std::cerr << "[config] warning: invalid wsPort=" << cfg.wsPort << ", using default 8080\n";
    cfg.wsPort = 8080;
  }
  if (cfg.feedPort <= 0 || cfg.feedPort > 65535) {
    std::cerr << "[config] warning: invalid feedPort=" << cfg.feedPort << ", using default 9001\n";
    cfg.feedPort = 9001;
  }

  unsigned short wsPort   = static_cast<unsigned short>(cfg.wsPort);
  unsigned short feedPort = static_cast<unsigned short>(cfg.feedPort);

  if (argc > 1) wsPort   = parsePort(argv[1], wsPort);
  if (argc > 3) feedPort = parsePort(argv[3], feedPort);

  // CLI args win over the config file — connectors read cfg.{wsPort,feedPort}.
  cfg.wsPort   = wsPort;
  cfg.feedPort = feedPort;

  // 2) Logger
  logger().log(
    LogLevel::Info,
    "boot",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", std::to_string(feedPort)}}
  );

  // 3) Engine bootstrap: register generic worker functions and node types.
  //    Connectors register their own event computers in registerWith().
  gma::registerBuiltinFunctions();
  gma::registerBuiltinNodeTypes();

  // 4) Thread pool (global)
  unsigned poolSize = cfg.threadPoolSize > 0
      ? static_cast<unsigned>(cfg.threadPoolSize)
      : std::thread::hardware_concurrency();
  if (poolSize == 0) poolSize = 4;
  gma::gThreadPool = std::make_shared<gma::rt::ThreadPool>(poolSize);
  shutdown.registerStep("pool-drain",   80, []{ if (gma::gThreadPool) gma::gThreadPool->drain(); });
  shutdown.registerStep("pool-destroy", 85, []{ gma::gThreadPool.reset(); });

  // 5) Core components
  auto store      = std::make_shared<gma::AtomicStore>();
  auto dispatcher = std::make_shared<gma::Dispatcher>(gma::gThreadPool.get(), store.get(), cfg);

  // 6) Metrics reporter
  if (cfg.metricsEnabled) {
    gma::util::MetricRegistry::instance().startReporter(
        static_cast<unsigned>(cfg.metricsIntervalSec));
    shutdown.registerStep("metrics-stop", 10, []{
      gma::util::MetricRegistry::instance().stopReporter();
    });
  }

  // 7) ASIO + engine WebSocket server
  boost::asio::io_context ioc;
  gma::ExecutionContext exec(store.get(), gma::gThreadPool.get());

  gma::WebSocketServer ws(ioc, &exec, dispatcher.get(), wsPort);
  ws.run();
  shutdown.registerStep("ws-stop-accept",    5,  [&ws]{ try { ws.stopAccept(); } catch (...) {} });
  shutdown.registerStep("ws-close-sessions", 40, [&ws]{ try { ws.closeAll(); } catch (...) {} });
  shutdown.registerStep("asio-stop",         60, [&ioc]{ try { ioc.stop(); } catch (...) {} });

  // 8) Connector registration — construct → registerWith → start, with a
  //    single ShutdownCoordinator step that calls stop() in reverse-registration
  //    order. With one connector today the reverse-order is trivial; the
  //    pattern is future-proof.
  gma::engine::EngineRegistries regs{
    &cfg, gma::gThreadPool.get(), store.get(), dispatcher.get(), &shutdown, &ioc,
    &gma::engine::EventTypeRegistry::singleton(),
    &gma::engine::EventComputerRegistry::singleton(),
    &gma::engine::NodeTypeRegistry::singleton(),
    &gma::engine::IngressRegistry::singleton(),
    &gma::engine::ConfigNamespaceRegistry::singleton(),
    &gma::AtomicProviderRegistry::singleton(),
    &gma::FunctionMap::instance(),
    &gma::util::logger(),
  };
  std::vector<gma::engine::IConnector*> connectors;
  gma::market::MarketConnector marketConnector;
  marketConnector.registerWith(regs);
  connectors.push_back(&marketConnector);
  // (future) gma::crypto::CoinbaseConnector{}.registerWith(regs);

  // Replay any config keys parked during cfg.loadFromFile() through
  // ConfigNamespaceRegistry now that connectors have registered their
  // namespaces. Ordering: load → registerWith → dispatchPendingKeys → start.
  cfg.dispatchPendingKeys();

  for (auto* c : connectors) c->start();
  shutdown.registerStep("connectors-stop", 30, [&connectors] {
    for (auto it = connectors.rbegin(); it != connectors.rend(); ++it) {
      (*it)->stop();
    }
  });

  logger().log(
    LogLevel::Info,
    "listening",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", std::to_string(feedPort)}}
  );

  // 9) Run
  try {
    ioc.run();
  } catch (const std::exception& ex) {
    logger().log(LogLevel::Error, std::string("io_context exception: ") + ex.what(), {});
  } catch (...) {
    logger().log(LogLevel::Error, "io_context exception: unknown", {});
  }

  shutdown.stop();
  g_shutdown.store(nullptr, std::memory_order_release);
  logger().log(LogLevel::Info, "stopped", {});
  return EXIT_SUCCESS;
}
