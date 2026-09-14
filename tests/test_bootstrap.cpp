// Registers engine builtins (worker functions and node types), constructs a
// process-static MarketConnector, and calls its registerWith() — NOT start().
// Tests don't need live sockets, so the feed server / WS clients are
// constructed but never started. The "tick" event computer factory is
// registered with EventComputerRegistry as a side effect of registerWith, so
// every Dispatcher built in any test picks up its own fresh MarketTickComputer
// the first time it sees a tick.
//
// ENC-1102: gtest calls Environment::SetUp() once per --gtest_repeat
// ITERATION, not once per process, while every registry it writes to is a
// process-global singleton. So this has to be idempotent — see the two
// comments inside SetUp().

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/NodeRegistry.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
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
  bool                          built = false;
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
    auto& g = globals();

    // Build the process-static objects exactly once. They are documented above
    // as living for the whole binary because the connector's registrations
    // close over them; rebuilding them on each --gtest_repeat iteration would
    // tear down a live ThreadPool/Dispatcher mid-binary for no gain.
    if (!g.built) {
      g.pool       = std::make_unique<gma::rt::ThreadPool>(1);
      g.store      = std::make_unique<gma::AtomicStore>();
      g.dispatcher = std::make_unique<gma::Dispatcher>(g.pool.get(), g.store.get(), g.cfg);
      g.shutdown   = std::make_unique<gma::rt::ShutdownCoordinator>();
      g.ioc        = std::make_unique<boost::asio::io_context>();
      g.market     = std::make_unique<gma::market::MarketConnector>();
      g.built      = true;
    }

    // Reset the one registry whose registration is ADDITIVE before re-running
    // the registrations below, so iteration N starts from the registry state a
    // fresh process would have.
    //
    // EventComputerRegistry::registerFactory appends by design ("Multiple
    // factories per type are retained in registration order") — so re-running
    // MarketConnector::registerWith() without this leaves iteration N with N
    // "tick" computers, i.e. every tick's atomics computed N times by N
    // independent MarketTickComputers with N independent histories.
    // tests/connectors/ConnectorLifecycleRealTest.cpp already documents that
    // exact corruption as the reason it must not call registerWith() twice;
    // --gtest_repeat was calling it again anyway, once per iteration.
    gma::engine::EventComputerRegistry::clear();

    // Everything else re-registered below is already idempotent, and re-running
    // it repairs any registry a test cleared:
    //   FunctionMap::registerFunction, AtomicProviderRegistry::registerNamespace
    //       -> replace in place
    //   NodeTypeRegistry::registerNodeType, EventTypeRegistry::registerEvent,
    //   IngressRegistry::registerIngress
    //       -> first-wins; the duplicate returns false and changes nothing
    gma::registerBuiltinFunctions();
    gma::registerBuiltinNodeTypes();

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
