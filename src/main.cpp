// File: src/main.cpp
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/signal_set.hpp>
#include <boost/system/system_error.hpp>

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

// -------- Forum-driven ingress --------
#include "gma/forum/ConnectorsClient.hpp"

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
// runServer — the real entry point. Wrapped by main() below, which turns any
// escaping exception into a clean, non-zero exit instead of std::terminate
// (ENC-1006).
// ---------------------------
static int runServer(int argc, char* argv[]) {
  using namespace gma::util;

  // Shutdown coordinator — declared early so it outlives servers.
  // Signal handling is wired below via boost::asio::signal_set, once the
  // io_context exists (async-signal-safe — see "0) Signals" near ioc).
  gma::rt::ShutdownCoordinator shutdown;

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

  // CLI args win over the config file — engine + connectors read these as-is.
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
  auto store = std::make_shared<gma::AtomicStore>();
  store->setCaps(static_cast<std::size_t>(std::max(0, cfg.maxSymbols)),
                 static_cast<std::size_t>(std::max(0, cfg.maxFieldsPerSymbol)));
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

  // 0) Signals -> graceful stop, delivered as a normal io_context completion
  //    handler. Async-signal-safe: the OS signal only marks the signal_set;
  //    ShutdownCoordinator::stop() (mutexes, logging, strand dispatch — none
  //    async-signal-safe) then runs from ioc.run()'s thread, a normal context.
  //    stop() is idempotent (internal CAS), so the post-run shutdown.stop()
  //    below is a harmless second call when a signal triggered the first.
  boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
  signals.async_wait([&shutdown](const boost::system::error_code& ec, int /*signo*/) {
    if (!ec) shutdown.stop();
  });

  gma::ExecutionContext exec(store.get(), gma::gThreadPool.get());

  // ENC-1006: the acceptor is opened in the constructor and throws on a
  // failed open/bind/listen. Catch it here so an occupied port produces an
  // actionable message + non-zero exit instead of terminate().
  std::unique_ptr<gma::WebSocketServer> wsPtr;
  try {
    wsPtr = std::make_unique<gma::WebSocketServer>(ioc, &exec, dispatcher.get(), wsPort);
  } catch (const boost::system::system_error& ex) {
    std::cerr << "[fatal] cannot start WebSocket server on port " << wsPort
              << ": " << ex.what() << "\n"
              << "        (is another gma_server already listening there? "
                 "pass a different wsPort as argv[1])\n";
    shutdown.stop();
    return EXIT_FAILURE;
  }
  gma::WebSocketServer& ws = *wsPtr;
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

  // ENC-31: ensure cfg.ingress[] is populated even when the user provided
  // no INI (loadFromFile not called). Idempotent.
  cfg.synthesizeIngressFromLegacy();

  // feed-sim-connector phase 2: when forumUrl is configured, replace
  // cfg.ingress with what forum's /api/connectors says. Connector
  // record is the source of truth; static INI ingress only matters
  // for forum-less dev runs (which now go through this fallback).
  // Env-var overrides apply when the INI didn't carry the keys.
  {
    auto envOr = [](const std::string& cfgVal, const char* envName) {
      if (!cfgVal.empty()) return cfgVal;
      const char* env = std::getenv(envName);
      return env ? std::string{env} : std::string{};
    };
    const std::string forumUrl       = envOr(cfg.forumUrl,       "FORUM_URL");
    const std::string forumTenantId  = envOr(cfg.forumTenantId,  "FORUM_TENANT_ID");
    const std::string forumAuthToken = envOr(cfg.forumAuthToken, "FORUM_AGENT_TOKEN");

    if (!forumUrl.empty()) {
      try {
        auto pulled = gma::forum::ConnectorsClient::fetchIngresses(
            forumUrl, forumTenantId, forumAuthToken);
        cfg.ingress = std::move(pulled);
        logger().log(
            LogLevel::Info,
            "forum.connectors.ingress_replaced",
            {{"count", std::to_string(cfg.ingress.size())}, {"forumUrl", forumUrl}});

        // Fail-fast per spec AC-6: a forum pull that parses cleanly but yields
        // zero ingress entries (forum returned [] or only rows that
        // parseConnectorsJson skipped — non-itch protocol / missing endpoint)
        // would otherwise boot a server with no data source, silently. Treat an
        // empty post-pull ingress as fatal, exactly like a transport failure.
        if (cfg.ingress.empty()) {
          logger().log(
              LogLevel::Error,
              "forum.connectors.empty_ingress",
              {{"forumUrl", forumUrl},
               {"detail",
                "forum returned no usable connectors; refusing to boot with "
                "empty ingress (AC-6)"}});
          std::exit(1);
        }
      } catch (const std::exception& ex) {
        logger().log(
            LogLevel::Error,
            "forum.connectors.fetch_failed",
            {{"err", ex.what()}, {"forumUrl", forumUrl}});
        // Fail-fast per spec AC-6: do not boot with empty ingress
        // and silently produce no data. The dev compose has forum
        // as a depends_on; hitting this almost always means a
        // misconfig the operator should see.
        std::exit(1);
      }
    }
  }

  // Replay any config keys parked during cfg.loadFromFile() through
  // ConfigNamespaceRegistry now that connectors have registered their
  // namespaces. Ordering: load → registerWith → dispatchPendingKeys
  // → ingress driver → connector start.
  cfg.dispatchPendingKeys();

  // Engine-driven ingress (ENC-31): read cfg.ingress[], lookup each kind in
  // IngressRegistry, instantiate, start in registration order. A single
  // ShutdownCoordinator step at priority 35 stops them in reverse order.
  std::vector<std::unique_ptr<gma::engine::IIngressSource>> ingresses;
  std::vector<std::string> ingressKinds;  // parallel to `ingresses` (skips are not pushed)
  for (const auto& entry : cfg.ingress) {
    const auto* factory = gma::engine::IngressRegistry::find(entry.kind);
    if (!factory) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
        "ingress.unknown_kind",
        {{"kind", entry.kind}});
      continue;
    }
    try {
      auto src = (*factory)(regs, entry.params);
      if (src) {
        ingresses.push_back(std::move(src));
        ingressKinds.push_back(entry.kind);
      }
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "ingress.factory_threw",
        {{"kind", entry.kind}, {"err", ex.what()}});
    }
  }
  // ENC-1006: start() opens the ingress socket (e.g. FeedServer's acceptor)
  // and throws on a failed bind. Report which ingress failed and exit
  // non-zero rather than letting the exception escape into terminate().
  for (std::size_t i = 0; i < ingresses.size(); ++i) {
    try {
      ingresses[i]->start();
    } catch (const std::exception& ex) {
      const std::string kind = i < ingressKinds.size() ? ingressKinds[i] : std::string{"?"};
      gma::util::logger().log(gma::util::LogLevel::Error, "ingress.start_failed",
                              {{"kind", kind}, {"err", ex.what()}});
      std::cerr << "[fatal] cannot start ingress '" << kind << "': " << ex.what() << "\n"
                << "        (is another gma_server already listening there? "
                   "pass a different feedPort as argv[3] / ingress.N.port)\n";
      // Stop the ingresses that did come up, then unwind cleanly.
      for (std::size_t j = i; j-- > 0;) {
        if (ingresses[j]) ingresses[j]->stop();
      }
      shutdown.stop();
      return EXIT_FAILURE;
    }
  }
  shutdown.registerStep("ingress-stop", 35, [&ingresses] {
    for (auto it = ingresses.rbegin(); it != ingresses.rend(); ++it) {
      if (*it) (*it)->stop();
    }
  });

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
  logger().log(LogLevel::Info, "stopped", {});
  return EXIT_SUCCESS;
}

// ---------------------------
// main — thin wrapper. Anything that still escapes runServer() (a bind we did
// not anticipate, a connector throwing at boot) is reported on stderr and
// exits non-zero. Deliberately no catch(...): a non-std throw should still
// abort loudly rather than be silently converted into an exit code.
// ---------------------------
int main(int argc, char* argv[]) {
  try {
    return runServer(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "[fatal] gma_server: " << ex.what() << "\n";
    return EXIT_FAILURE;
  }
}
