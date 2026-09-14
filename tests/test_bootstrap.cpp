// Runs once per test-binary boot. Registers engine builtins (worker functions
// and node types), constructs a process-static MarketConnector, and calls its
// registerWith() — NOT start(). Tests don't need live sockets, so the feed
// server / WS clients are constructed but never started. The "tick" event
// computer factory is registered with EventComputerRegistry as a side effect
// of registerWith, so every Dispatcher built in any test picks up its own
// fresh MarketTickComputer the first time it sees a tick.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/Registries.hpp"
#include "gma/market/MarketConnector.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <memory>

namespace {

// Process-static singletons so the connector's captured pointers stay valid
// for the entire test-binary lifetime.
struct TestFixtureGlobals {
  gma::util::Config             cfg;
  std::unique_ptr<gma::rt::ThreadPool>   pool;
  std::unique_ptr<gma::AtomicStore>      store;
  std::unique_ptr<gma::Dispatcher>       dispatcher;
  std::unique_ptr<gma::rt::ShutdownCoordinator> shutdown;
  std::unique_ptr<boost::asio::io_context> ioc;
  std::unique_ptr<gma::market::MarketConnector> market;
};

TestFixtureGlobals& globals() {
  static TestFixtureGlobals g;
  return g;
}

class BuiltinsEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    gma::registerBuiltinFunctions();
    gma::registerBuiltinNodeTypes();

    auto& g = globals();
    g.pool       = std::make_unique<gma::rt::ThreadPool>(1);
    g.store      = std::make_unique<gma::AtomicStore>();
    g.dispatcher = std::make_unique<gma::Dispatcher>(g.pool.get(), g.store.get(), g.cfg);
    g.shutdown   = std::make_unique<gma::rt::ShutdownCoordinator>();
    g.ioc        = std::make_unique<boost::asio::io_context>();
    g.market     = std::make_unique<gma::market::MarketConnector>();

    gma::engine::EngineRegistries regs{
      &g.cfg, g.pool.get(), g.store.get(), g.dispatcher.get(),
      g.shutdown.get(), g.ioc.get(),
      &gma::engine::EventTypeRegistry::singleton(),
      &gma::engine::EventComputerRegistry::singleton(),
      &gma::engine::NodeTypeRegistry::singleton(),
      &gma::engine::IngressRegistry::singleton(),
      &gma::engine::ConfigNamespaceRegistry::singleton(),
      &gma::AtomicProviderRegistry::singleton(),
      &gma::FunctionMap::instance(),
      &gma::util::logger(),
    };

    g.market->registerWith(regs);   // NOTE: no start() — tests do not run sockets.
  }
};

::testing::Environment* const kBuiltinsEnv =
    ::testing::AddGlobalTestEnvironment(new BuiltinsEnvironment);

} // namespace
