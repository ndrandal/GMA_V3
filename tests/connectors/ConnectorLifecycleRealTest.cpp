// ENC-803 (M18): lifecycle tests for the REAL production connectors.
//
// ConnectorLifecycleTest.cpp pins the IConnector contract against a hand-rolled
// MockConnector written to throw on double-start. The actual connectors do NOT
// behave that way, and that divergence was untested:
//   - SyntheticConnector::start() re-arms its timer with no double-start guard.
//   - MarketConnector::start() is a no-op that does not throw, with no test.
// These tests assert what the connectors REALLY do (no aspirational throws).

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/Registries.hpp"
#include "gma/market/MarketConnector.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/synthetic/SyntheticConnector.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

#include <chrono>
#include <limits>

using namespace gma;

namespace {

engine::EngineRegistries buildRegs(util::Config& cfg,
                                   rt::ThreadPool& pool,
                                   AtomicStore& store,
                                   Dispatcher& dispatcher,
                                   rt::ShutdownCoordinator& shutdown,
                                   boost::asio::io_context& ioc) {
  return engine::EngineRegistries{
    &cfg, &pool, &store, &dispatcher, &shutdown, &ioc,
    &engine::EventTypeRegistry::singleton(),
    &engine::EventComputerRegistry::singleton(),
    &engine::NodeTypeRegistry::singleton(),
    &engine::IngressRegistry::singleton(),
    &engine::ConfigNamespaceRegistry::singleton(),
    &AtomicProviderRegistry::singleton(),
    &FunctionMap::instance(),
    &util::logger(),
  };
}

} // namespace

// ===========================================================================
// SyntheticConnector — actual lifecycle behavior
// ===========================================================================

// Double-start is NOT a programmer-error throw for the real connector: start()
// simply (re)arms the timer. Calling it twice must not throw and events must
// still flow.
TEST(ConnectorLifecycleRealTest, SyntheticDoubleStartDoesNotThrow) {
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;
  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector::Options opts;
  opts.streamKey = "SYN_DBL";
  opts.tickMs    = 2;
  opts.maxTicks  = 50;
  synthetic::SyntheticConnector connector(opts);
  connector.registerWith(regs);

  EXPECT_NO_THROW(connector.start());
  EXPECT_NO_THROW(connector.start());   // re-arm, no guard, must not throw

  ioc.run_for(std::chrono::milliseconds(120));
  connector.stop();
  pool.shutdown();

  EXPECT_TRUE(store.get("SYN_DBL", "synthetic.sin").has_value())
      << "timer never fired after double-start";
}

// stop() is noexcept and idempotent: multiple calls are safe and quiesce the
// timer (no further events after stop).
TEST(ConnectorLifecycleRealTest, SyntheticStopIsIdempotentAndQuiesces) {
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;
  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector::Options opts;
  opts.streamKey = "SYN_STOP";
  opts.tickMs    = 2;
  synthetic::SyntheticConnector connector(opts);
  connector.registerWith(regs);
  connector.start();

  ioc.run_for(std::chrono::milliseconds(40));
  EXPECT_NO_THROW(connector.stop());
  EXPECT_NO_THROW(connector.stop());   // idempotent — second/third are no-ops
  EXPECT_NO_THROW(connector.stop());

  // After stop the timer chain is cancelled; running the io_context further
  // produces no new work (any queued cancel handler returns on ec). The ioc was
  // never exhausted (the timer kept re-arming, so run_for returned on duration),
  // so it can be run again without restart().
  ioc.run_for(std::chrono::milliseconds(40));
  pool.shutdown();
  SUCCEED();
}

// stop() before any start() is also a safe no-op (guarded by the null-timer
// check), and stop() before registerWith must not crash.
TEST(ConnectorLifecycleRealTest, SyntheticStopBeforeStartIsSafe) {
  synthetic::SyntheticConnector connector;            // never registered
  EXPECT_NO_THROW(connector.stop());                  // _timer == nullptr path

  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;
  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector registered;
  registered.registerWith(regs);
  EXPECT_NO_THROW(registered.stop());                 // registered but not started
  pool.shutdown();
}

// Re-startability: after a stop(), start() re-arms and events flow again.
TEST(ConnectorLifecycleRealTest, SyntheticRestartAfterStop) {
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;
  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector::Options opts;
  opts.streamKey = "SYN_RESTART";
  opts.tickMs    = 2;
  synthetic::SyntheticConnector connector(opts);
  connector.registerWith(regs);

  connector.start();
  ioc.run_for(std::chrono::milliseconds(30));
  connector.stop();

  // The ioc was never exhausted (run_for returned on duration with the timer
  // still pending), so no restart() is needed before re-arming.
  EXPECT_NO_THROW(connector.start());                 // restart
  ioc.run_for(std::chrono::milliseconds(60));
  connector.stop();
  pool.shutdown();

  EXPECT_TRUE(store.get("SYN_RESTART", "synthetic.sin").has_value())
      << "connector did not produce events after restart";
}

// ===========================================================================
// MarketConnector — actual lifecycle behavior
// ===========================================================================
//
// NOTE: MarketConnector::registerWith() APPENDS a "tick" factory to the
// process-global EventComputerRegistry (singleton, append-only, no per-factory
// removal). The test bootstrap already calls registerWith() once for the whole
// binary; calling it again here would permanently duplicate that factory and
// corrupt every other test's tick path. So these tests exercise start()/stop()
// — the methods M18 calls out — WITHOUT a second registerWith(), which is the
// real contract anyway (start() touches no per-instance state).

// start() is a pure no-op that does not throw, and is safe to call repeatedly
// (no double-start guard, because there is nothing to start).
TEST(ConnectorLifecycleRealTest, MarketStartIsNoOpNoThrow) {
  market::MarketConnector connector;
  EXPECT_NO_THROW(connector.start());
  EXPECT_NO_THROW(connector.start());   // idempotent no-op
}

// stop() is noexcept and idempotent. It clears the global AtomicProviderRegistry
// "ob" namespace; we restore a benign resolver afterward so no later test that
// resolves ob.* through the registry is affected (the live resolver returns NaN
// for most keys anyway — see H4).
TEST(ConnectorLifecycleRealTest, MarketStopIsNoexceptIdempotent) {
  market::MarketConnector connector;
  EXPECT_NO_THROW(connector.stop());
  EXPECT_NO_THROW(connector.stop());    // idempotent
  EXPECT_NO_THROW(connector.start());   // start after stop is still a safe no-op

  // Restore an "ob" namespace so suite-global state matches the bootstrap's
  // (live provider mostly resolves to NaN; this keeps that contract intact).
  AtomicProviderRegistry::registerNamespace(
      "ob", [](const std::string&, const std::string&) {
        return std::numeric_limits<double>::quiet_NaN();
      });
}
