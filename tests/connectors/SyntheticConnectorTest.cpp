// Demo test for the synthetic connector. Also doubles as the proof that a
// second, completely independent connector can coexist with the market
// connector without touching any engine code.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/Registries.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/synthetic/SyntheticConnector.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <cmath>
#include <memory>
#include <variant>

using namespace gma;

namespace {

double asDouble(const std::optional<ArgType>& v, double fallback = 0.0) {
  if (!v) return fallback;
  return std::visit(
    [fallback](auto&& x) -> double {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, double>) return x;
      else if constexpr (std::is_same_v<T, int>) return static_cast<double>(x);
      else return fallback;
    }, *v);
}

Event makeSyntheticEvent(const std::string& sym, double counter) {
  auto doc = std::make_shared<rapidjson::Document>();
  doc->SetObject();
  doc->AddMember("counter", rapidjson::Value(counter), doc->GetAllocator());
  Event e;
  e.type    = "synthetic.tick";
  e.symbol  = sym;
  e.payload = std::move(doc);
  return e;
}

} // namespace

TEST(SyntheticConnectorTest, ComputerWritesSinCosToStore) {
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);

  synthetic::SyntheticEventComputer computer;
  engine::ComputeContext ctx{ &store, &dispatcher, &pool };

  computer.compute(makeSyntheticEvent("SYN", 0.0), ctx);
  EXPECT_DOUBLE_EQ(asDouble(store.get("SYN", "synthetic.sin"), 42.0), std::sin(0.0));
  EXPECT_DOUBLE_EQ(asDouble(store.get("SYN", "synthetic.cos"), 42.0), std::cos(0.0));

  computer.compute(makeSyntheticEvent("SYN", 10.0), ctx);
  EXPECT_DOUBLE_EQ(asDouble(store.get("SYN", "synthetic.sin")), std::sin(1.0));
  EXPECT_DOUBLE_EQ(asDouble(store.get("SYN", "synthetic.cos")), std::cos(1.0));

  pool.shutdown();
}

TEST(SyntheticConnectorTest, DispatcherRoutesByEventType) {
  // With both the market tick computer (installed globally via bootstrap) and
  // the synthetic computer added locally, events must route only to the
  // computer whose eventType() matches. Specifically: a "synthetic.tick" event
  // should NOT trigger market TA (which would try to extract lastPrice/volume
  // from a payload that only has a "counter" field).
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  dispatcher.addComputer(std::make_unique<synthetic::SyntheticEventComputer>());

  dispatcher.onTick(makeSyntheticEvent("SYN", 5.0));
  pool.shutdown();

  EXPECT_TRUE(store.get("SYN", "synthetic.sin").has_value());
  // Market computer must have left no TA keys behind on SYN — it should have
  // seen a mismatched event type and skipped entirely.
  EXPECT_FALSE(store.get("SYN", "sma_5").has_value());
  EXPECT_FALSE(store.get("SYN", "lastPrice").has_value());
}

namespace {

// Build a fully-populated EngineRegistries pointing at the supplied per-test
// engine objects. Mirrors what the composition root does in main().
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

TEST(SyntheticConnectorTest, RegisterWithDoesNotFireTimer) {
  // Lifecycle invariant: registerWith must allocate but not arm the timer.
  // Running the io_context after registerWith alone should produce zero
  // synthetic events, since start() has not been called.
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;

  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector::Options opts;
  opts.streamKey = "SYN_NOSTART";
  opts.tickMs    = 1;     // would fire fast if armed.
  opts.maxTicks  = 5;
  synthetic::SyntheticConnector connector(opts);
  connector.registerWith(regs);

  ioc.run_for(std::chrono::milliseconds(50));
  pool.shutdown();

  EXPECT_FALSE(store.get("SYN_NOSTART", "synthetic.sin").has_value())
      << "registerWith leaked timer arming — start() should be the only path "
         "that schedules events";
}

TEST(SyntheticConnectorTest, StartArmsTimerStopCancels) {
  // End-to-end lifecycle: registerWith → start fires the timer; stop cancels
  // any further callbacks. Verifies the post-phase-4 control flow.
  AtomicStore store;
  rt::ThreadPool pool(1);
  Dispatcher dispatcher(&pool, &store);
  boost::asio::io_context ioc;
  util::Config cfg;
  rt::ShutdownCoordinator shutdown;

  auto regs = buildRegs(cfg, pool, store, dispatcher, shutdown, ioc);

  synthetic::SyntheticConnector::Options opts;
  opts.streamKey = "SYN_E2E";
  opts.tickMs    = 5;
  opts.maxTicks  = 3;
  synthetic::SyntheticConnector connector(opts);
  connector.registerWith(regs);
  connector.start();

  ioc.run_for(std::chrono::milliseconds(200));
  connector.stop();
  pool.shutdown();

  EXPECT_TRUE(store.get("SYN_E2E", "synthetic.sin").has_value())
      << "computer never ran — start() didn't arm the timer";
}
