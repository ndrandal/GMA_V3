// Pins the shape of the EngineRegistries struct: every field a connector
// might rely on must be populatable. The composition root in main.cpp wires
// real instances; this test wires every field with non-null pointers and
// verifies they round-trip.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/atomic/AtomicProviderRegistry.hpp"
#include "gma/engine/Registries.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/runtime/ShutdownCoordinator.hpp"
#include "gma/util/Config.hpp"
#include "gma/util/Logger.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>

using namespace gma;

TEST(EngineRegistriesShapeTest, AllFieldsPopulatable) {
  util::Config cfg;
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  rt::ShutdownCoordinator shutdown;
  boost::asio::io_context ioc;

  engine::EngineRegistries regs{
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

  // Per-instance engine objects.
  EXPECT_EQ(regs.cfg, &cfg);
  EXPECT_EQ(regs.pool, &pool);
  EXPECT_EQ(regs.store, &store);
  EXPECT_EQ(regs.dispatcher, &dispatcher);
  EXPECT_EQ(regs.shutdown, &shutdown);
  EXPECT_EQ(regs.io, &ioc);

  // Extension-point registries.
  EXPECT_NE(regs.events,    nullptr);
  EXPECT_NE(regs.computers, nullptr);
  EXPECT_NE(regs.nodes,     nullptr);
  EXPECT_NE(regs.ingress,   nullptr);
  EXPECT_NE(regs.configNs,  nullptr);
  EXPECT_NE(regs.providers, nullptr);
  EXPECT_NE(regs.functions, nullptr);
  EXPECT_NE(regs.log,       nullptr);

  // Singleton accessors must return stable references across calls.
  EXPECT_EQ(&engine::EventTypeRegistry::singleton(),     &engine::EventTypeRegistry::singleton());
  EXPECT_EQ(&engine::EventComputerRegistry::singleton(), &engine::EventComputerRegistry::singleton());
  EXPECT_EQ(&engine::NodeTypeRegistry::singleton(),      &engine::NodeTypeRegistry::singleton());
  EXPECT_EQ(&engine::IngressRegistry::singleton(),       &engine::IngressRegistry::singleton());
  EXPECT_EQ(&engine::ConfigNamespaceRegistry::singleton(),&engine::ConfigNamespaceRegistry::singleton());
  EXPECT_EQ(&AtomicProviderRegistry::singleton(),        &AtomicProviderRegistry::singleton());
}

TEST(EngineRegistriesShapeTest, FieldCountIs14) {
  // Guard against accidental field removal: sizeof should equal 14 pointers.
  // (Six per-instance + eight registry pointers, all 8-byte on x86_64.)
  static_assert(sizeof(engine::EngineRegistries) == 14 * sizeof(void*),
                "EngineRegistries must carry exactly 14 pointer-sized fields");
  SUCCEED();
}
