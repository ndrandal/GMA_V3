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
#include "gma/server/WebSocketServer.hpp"   // your existing WS transport
#include "gma/server/FeedServer.hpp"        // optional external feed ingress

// -------- Utilities from T6–T8 --------
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/ExecutionContext.hpp"

// -------- TA (T3) --------
#include "gma/ta/TAComputer.hpp"
#include "gma/ta/AtomicNames.hpp"

// -------- OB (T4) --------
#include "gma/atomic/AtomicProveiderRegistry.hpp"
// If you enabled the order book provider/materializer in your tree, keep these includes;
// if not yet, you can temporarily comment them out.
#include "gma/ob/FunctionalSnapshotSource.hpp"
#include "gma/ob/ObProvider.hpp"
#include "gma/ob/ObMaterializer.hpp"
#include "gma/ob/ObKeysCatalog.hpp"

// ---------------------------
// Helpers
// ---------------------------
static inline int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


// Global shutdown coordinator (T7)
static gma::rt::ShutdownCoordinator gShutdown;

// Simple POSIX signal hook to trigger graceful stop (T7)
static void handleSignal(int) {
  gShutdown.stop();
}

// ---------------------------
// “Build” helpers (adapt to your codebase)
// ---------------------------

// If you already have factories, replace these with your real ones.
static std::shared_ptr<gma::AtomicStore> makeAtomicStore() {
  // Your store likely needs config, etc. Keep it simple here:
  return std::make_shared<gma::AtomicStore>();
}


static std::shared_ptr<gma::MarketDispatcher>
makeDispatcher(const std::shared_ptr<gma::AtomicStore>& store)
{
  // MarketDispatcher wants (ThreadPool*, AtomicStore*)
  // ThreadPool type is gma::rt::ThreadPool in your repo.
  return std::make_shared<gma::MarketDispatcher>(gma::gThreadPool.get(), store.get());
}

// ---------------------------
// main
// ---------------------------
int main(int argc, char* argv[]) {
  using namespace gma::util;

  // ---------------------------
  // 0) Signals -> graceful stop
  // ---------------------------
  std::signal(SIGINT,  handleSignal);
  std::signal(SIGTERM, handleSignal);

  // ---------------------------
  // 1) Config (your current Config only supports loadFromFile)
  //    argv[1] = wsPort (optional)
  //    argv[2] = configFilePath (optional)
  // ---------------------------
  Config cfg; // NOTE: no Config::get() in your current header

  unsigned short wsPort = 8080; // sensible default
  if (argc > 1) {
    try {
      wsPort = static_cast<unsigned short>(std::stoul(argv[1]));
    } catch (...) {
      std::cerr << "Invalid port '" << argv[1] << "', using default " << wsPort << "\n";
    }
  }

  if (argc > 2) {
    if (!cfg.loadFromFile(argv[2])) {
      std::cerr << "[config] warning: failed to load file: " << argv[2] << "\n";
    }
  }

  // ---------------------------
  // 2) Logger (no macros in your build)
  // ---------------------------
  logger().log(
    gma::util::LogLevel::Info,
    "boot",
    {{"wsPort", std::to_string(wsPort)}}
  );

  // ---------------------------
  // 3) Thread pool (global)
  // ---------------------------
  gma::gThreadPool = std::make_shared<gma::rt::ThreadPool>(4); // choose a default
  gShutdown.registerStep("pool-drain",   80, []{ if (gma::gThreadPool) gma::gThreadPool->drain(); });
  gShutdown.registerStep("pool-destroy", 85, []{ gma::gThreadPool.reset(); });

  // ---------------------------
  // 4) Core components
  // ---------------------------
  auto store      = makeAtomicStore();
  auto dispatcher = makeDispatcher(store);

  // ---------------------------
  // 5) ASIO + servers
  // ---------------------------
  boost::asio::io_context ioc;

  // ExecutionContext wants (AtomicStore*, ThreadPool*)
  // (you already fixed the ctor error, so this should match now)
  gma::ExecutionContext exec(store.get(), gma::gThreadPool.get());

  gma::WebSocketServer ws(ioc, &exec, dispatcher.get(), wsPort);
  ws.run();

  // Optional feed server (your header shows gma::FeedServer, takes MarketDispatcher*)
  gma::FeedServer feed(ioc, dispatcher.get(), 9001);
  feed.run();

  // ---------------------------
  // 6) Shutdown sequencing
  // ---------------------------
  gShutdown.registerStep("ws-stop-accept",    5,  [&ws]{ try { ws.stopAccept(); } catch (...) {} });
  gShutdown.registerStep("ws-close-sessions", 40, [&ws]{ try { ws.closeAll(); } catch (...) {} });

  gShutdown.registerStep("feed-stop",         55, [&feed]{ try { feed.stop(); } catch (...) {} });
  gShutdown.registerStep("asio-stop",         60, [&ioc]{ try { ioc.stop(); } catch (...) {} });

  logger().log(
    gma::util::LogLevel::Info,
    "listening",
    {{"wsPort", std::to_string(wsPort)}, {"feedPort", "9001"}}
  );

  // ---------------------------
  // 7) Run
  // ---------------------------
  try {
    ioc.run();
  } catch (const std::exception& ex) {
    logger().log(gma::util::LogLevel::Error, std::string("io_context exception: ") + ex.what(), {});
  } catch (...) {
    logger().log(gma::util::LogLevel::Error, "io_context exception: unknown", {});
  }

  // Ensure shutdown steps run even on natural exit
  gShutdown.stop();

  logger().log(gma::util::LogLevel::Info, "stopped", {});
  return EXIT_SUCCESS;
}

