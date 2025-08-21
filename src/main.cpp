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

// -------- TA (T3) --------
#include "gma/ta/TAComputer.hpp"
#include "gma/ta/AtomicNames.hpp"

// -------- OB (T4) --------
#include "gma/atomic/AtomicProviderRegistry.hpp"
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
  gShutdown.stopAll();
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
makeDispatcher(const std::shared_ptr<gma::AtomicStore>& store) {
  // Your dispatcher probably needs the pool and store; we use the global pool (T6).
  return std::make_shared<gma::MarketDispatcher>(gma::gThreadPool, store);
}

// If you built T5’s ClientConnection layer into WebSocketServer, you can keep using it as-is.
// This function demonstrates starting your transport exactly like your pre-change main did.
static void startWebSocketServer(
    boost::asio::io_context& ioc,
    const std::shared_ptr<gma::AtomicStore>& store,
    const std::shared_ptr<gma::MarketDispatcher>& dispatcher,
    unsigned short port)
{
  // If your WebSocketServer ctor signature differs, adjust here.
  gma::WebSocketServer server(ioc, store.get(), dispatcher.get(), port);
  server.run();
}

// Optional external feed ingress (if you use it). Adjust/remove if unused.
static void startFeedServer(
    boost::asio::io_context& ioc,
    const std::shared_ptr<gma::MarketDispatcher>& dispatcher,
    unsigned short port)
{
  server::FeedServer feedSrv{ioc, *dispatcher, port};
  feedSrv.run();
}

// ---------------------------
// main
// ---------------------------
int main(int argc, char* argv[]) {
  using namespace gma::util;

  // 0) OS signals -> graceful stop (T7)
  std::signal(SIGINT,  handleSignal);
  std::signal(SIGTERM, handleSignal);

  // 1) Load config (T8). Env-only works; set GMA_CONFIG to load a JSON file first.
  Config& cfg = Config::get();
  if (auto p = Config::env("GMA_CONFIG")) {
    std::string err; Config::loadFromFile(cfg, *p, &err);
    if (!err.empty()) std::cerr << "[config] warning: " << err << std::endl;
  }
  Config::loadFromEnv(cfg);

  // Allow port override from argv[1] for parity with your old main
  unsigned short wsPort = static_cast<unsigned short>(cfg.wsPort);
  if (argc > 1) {
    try { wsPort = static_cast<unsigned short>(std::stoul(argv[1])); }
    catch (...) {
      std::cerr << "Invalid port '" << argv[1] << "'. Using config/default " << wsPort << "\n";
    }
  }

  // 2) Logger & Metrics (T8)
  logger().setLevel(parseLevel(cfg.logLevel));
  logger().setFormatJson(cfg.logFormat == std::string("json"));
  logger().setFile(cfg.logFile);
  GLOG_INFO("boot", {{"wsPort", std::to_string(wsPort)},
                     {"threads", std::to_string(cfg.threadPoolSize)}});

  if (cfg.metricsEnabled) {
    MetricRegistry::instance().startReporter(cfg.metricsIntervalSec);
    gShutdown.registerStep("metrics-stop", 70, []{
      MetricRegistry::instance().stopReporter();
    });
  }

  // 3) ThreadPool (global) (T6)
  gma::gThreadPool = std::make_shared<gma::rt::ThreadPool>(cfg.threadPoolSize);
  gShutdown.registerStep("pool-drain",   80, []{ if (gma::gThreadPool) gma::gThreadPool->drain(); });
  gShutdown.registerStep("pool-destroy", 85, []{ gma::gThreadPool.reset(); });

  // 4) Core components: Store + Dispatcher
  auto store       = makeAtomicStore();
  auto dispatcher  = makeDispatcher(store);

  // 5) TAComputer (T3): compute px.* and write to AtomicStore (+ optional notify)
  gma::ta::TAConfig taCfg;
  taCfg.historyMax = cfg.taHistoryMax;
  taCfg.smaPeriods = cfg.taSMA;
  taCfg.emaPeriods = cfg.taEMA;
  taCfg.vwapPeriods= cfg.taVWAP;
  taCfg.medPeriods = cfg.taMED;
  taCfg.minPeriods = cfg.taMIN;
  taCfg.maxPeriods = cfg.taMAX;
  taCfg.stdPeriods = cfg.taSTD;
  taCfg.rsiPeriod  = cfg.taRSI;

  auto writeFn = [store](const std::string& sym, const std::string& key, double v, int64_t ts){
    // Implement your store write call:
    // store->write(sym, key, v, ts);
    (void)sym; (void)key; (void)v; (void)ts;
  };
  auto notifyFn = [dispatcher](const std::string& sym, const std::string& key){
    // If your dispatcher supports waking listeners per (sym,key), call it here:
    // dispatcher->notify(sym, key);
    (void)sym; (void)key;
  };
  auto ta = std::make_shared<gma::ta::TAComputer>(taCfg, writeFn, notifyFn);

  // 6) Order Book (T4): snapshot source + provider + materializer (optional)
  // Hook these lambdas into YOUR live order book manager (replace the TODOs).
  std::shared_ptr<gma::ob::FunctionalSnapshotSource> snapshotSrc;
  {
    // ---- Replace this block with your real capture/tick hooks ----
    auto captureFn = /* TODO: wire to your OrderBookManager */ 
      [](const std::string& sym, size_t maxLevels, gma::ob::Mode mode,
         std::optional<std::pair<double,double>> band) -> gma::ob::Snapshot {
        (void)sym; (void)maxLevels; (void)mode; (void)band;
        return {}; // Empty snapshot means OB provider is effectively disabled.
      };
    auto tickFn = /* TODO: return instrument tick size for 'sym' */ 
      [](const std::string& sym)->double { (void)sym; return 0.01; };
    snapshotSrc = std::make_shared<gma::ob::FunctionalSnapshotSource>(captureFn, tickFn);
  }

  // Register "ob" namespace so AtomicAccessor can lazy-resolve on cache miss.
  // If your captureFn is still the placeholder above, this will just return NaN.
  auto obProvider = std::make_shared<gma::ob::Provider>(snapshotSrc, /*per*/20, /*agg*/20);
  gma::AtomicProviderRegistry::registerNamespace(
    "ob",
    [obProvider](const std::string& sym, const std::string& fullKey){
      return obProvider->get(sym, fullKey);
    }
  );

  // Optional materialization (push hot keys into the store + notify). Safe to enable even if OB is sparse.
  auto obMat = std::make_shared<gma::ob::Materializer>(snapshotSrc, writeFn, notifyFn);
  gma::ob::MaterializeConfig obCfg;
  obCfg.defaultKeys     = gma::ob::defaultProfile(); // TOB, spread, cum depths, etc.
  obCfg.maxLevelsPer    = 20;
  obCfg.maxLevelsAgg    = 20;
  obCfg.throttleMs      = 5;
  obCfg.notifyOnWrite   = true;
  obMat->start(obCfg);
  gShutdown.registerStep("ob-materializer", 20, [obMat]{ obMat->stop(); });

  // 7) I/O (ASIO) + servers
  boost::asio::io_context ioc;

  // WebSocket transport (T5). This keeps your existing server but all request
  // trees ultimately hit AtomicAccessor/WSResponder you wired in T5.
  startWebSocketServer(ioc, store, dispatcher, wsPort);

  // If you have an external feed server that injects ticks or book deltas, start it here.
  // (Set the port from config or keep the legacy 9001 as your code had.)
  startFeedServer(ioc, dispatcher, /*port=*/9001);

  GLOG_INFO("listening", {{"wsPort", std::to_string(wsPort)}, {"feedPort", "9001"}});

  // 8) Shutdown sequencing (T7): stop accept first, then sessions (handled inside servers),
  //    then producers/timers/materializers, then thread pool, then metrics.
  // Your WebSocketServer/FeedServer should stop when io_context stops.
  gShutdown.registerStep("asio-stop", 60, [&ioc]{ ioc.stop(); });

  // 9) Run event loop (blocks until shutdown). If something else breaks the loop, ensure stopAll.
  try {
    ioc.run();
  } catch (const std::exception& ex) {
    GLOG_ERROR(std::string("io_context exception: ") + ex.what(), {});
  } catch (...) {
    GLOG_ERROR("io_context exception: unknown", {});
  }

  // 10) Make sure we stop everything on natural exit as well (idempotent).
  gShutdown.stopAll();
  GLOG_INFO("stopped", {});
  return EXIT_SUCCESS;
}
