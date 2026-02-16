// File: src/main.cpp
#include <csignal>
#include <cstdlib>
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

// -------- OB --------
#include "gma/atomic/AtomicProveiderRegistry.hpp"
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObProvider.hpp"
#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObKeysCatalog.hpp"

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

  // 3) Thread pool (global)
  unsigned poolSize = std::thread::hardware_concurrency();
  if (poolSize == 0) poolSize = 4;
  gma::gThreadPool = std::make_shared<gma::rt::ThreadPool>(poolSize);
  shutdown.registerStep("pool-drain",   80, []{ if (gma::gThreadPool) gma::gThreadPool->drain(); });
  shutdown.registerStep("pool-destroy", 85, []{ gma::gThreadPool.reset(); });

  // 4) Core components
  auto store      = std::make_shared<gma::AtomicStore>();
  auto dispatcher = std::make_shared<gma::MarketDispatcher>(gma::gThreadPool.get(), store.get());

  // 5) ASIO + servers
  boost::asio::io_context ioc;

  gma::ExecutionContext exec(store.get(), gma::gThreadPool.get());

  gma::WebSocketServer ws(ioc, &exec, dispatcher.get(), wsPort);
  ws.run();

  gma::FeedServer feed(ioc, dispatcher.get(), feedPort);
  feed.run();

  // 6) Shutdown sequencing — all captured objects are still alive when shutdown runs
  shutdown.registerStep("ws-stop-accept",    5,  [&ws]{ try { ws.stopAccept(); } catch (...) {} });
  shutdown.registerStep("ws-close-sessions", 40, [&ws]{ try { ws.closeAll(); } catch (...) {} });
  shutdown.registerStep("feed-stop",         55, [&feed]{ try { feed.stop(); } catch (...) {} });
  shutdown.registerStep("asio-stop",         60, [&ioc]{ try { ioc.stop(); } catch (...) {} });

  logger().log(
    LogLevel::Info,
    "listening",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", std::to_string(feedPort)}}
  );

  // 7) Run
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
