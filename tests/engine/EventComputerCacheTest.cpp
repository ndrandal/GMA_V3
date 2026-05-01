// Pins the lazy-cache contract in Dispatcher::onTick: a Dispatcher constructed
// before a factory is registered must still pick up that factory the first
// time it sees an event of that type. This is the single biggest invariant
// the EventComputerRegistry path provides over the old DefaultComputerFactory
// hook (which only ran in the ctor).
//
// Each test uses a unique event type so registry pollution doesn't leak
// between tests; we never call EventComputerRegistry::clear() (which would
// wipe the test-bootstrap's "tick" factory and break unrelated tests).

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <string>
#include <rapidjson/document.h>

using namespace gma;

namespace {

class CountingComputer final : public engine::IEventComputer {
public:
  explicit CountingComputer(std::string type, std::atomic<int>* invocations)
    : _type(std::move(type)), _invocations(invocations) {}
  std::string_view eventType() const override { return _type; }
  void compute(const Event&, engine::ComputeContext&) override {
    if (_invocations) _invocations->fetch_add(1);
  }
private:
  std::string _type;
  std::atomic<int>* _invocations;
};

Event makeEvent(const std::string& type, const std::string& sym) {
  Event e;
  e.type    = type;
  e.symbol  = sym;
  e.payload = std::make_shared<rapidjson::Document>();
  e.payload->SetObject();
  return e;
}

// Each test gets a unique event-type string so factories don't bleed across
// tests via the (process-wide) EventComputerRegistry singleton.
std::string uniqueType(const char* tag) {
  static std::atomic<int> counter{0};
  return std::string("cache.test.") + tag + "." + std::to_string(counter.fetch_add(1));
}

} // namespace

TEST(EventComputerCacheTest, LateRegisteredFactoryIsPickedUp) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);

  // Dispatcher is fully constructed BEFORE we register the factory.
  // Old behavior: the dispatcher's ctor took a snapshot via the static hook,
  // so anything registered after construction would never fire. New behavior:
  // the per-type cache is built lazily on first event, so this must work.
  const std::string evType = uniqueType("late");
  std::atomic<int> factoryCalls{0};
  std::atomic<int> invocations{0};

  engine::EventComputerRegistry::registerFactory(evType, [&] {
    factoryCalls.fetch_add(1);
    return std::make_unique<CountingComputer>(evType, &invocations);
  });

  EXPECT_EQ(factoryCalls.load(), 0);  // factory not invoked yet — lazy.
  dispatcher.onTick(makeEvent(evType, "X"));
  EXPECT_EQ(factoryCalls.load(), 1);  // first event of type → factory called.
  EXPECT_EQ(invocations.load(), 1);

  dispatcher.onTick(makeEvent(evType, "X"));
  EXPECT_EQ(factoryCalls.load(), 1);  // second event → cache hit, no new factory call.
  EXPECT_EQ(invocations.load(), 2);   // same instance fired twice.
}

TEST(EventComputerCacheTest, EachDispatcherGetsFreshInstances) {
  rt::ThreadPool pool(1);
  AtomicStore store;

  const std::string evType = uniqueType("fresh");
  std::atomic<int> factoryCalls{0};
  std::atomic<int> invocations{0};

  engine::EventComputerRegistry::registerFactory(evType, [&] {
    factoryCalls.fetch_add(1);
    return std::make_unique<CountingComputer>(evType, &invocations);
  });

  Dispatcher d1(&pool, &store);
  Dispatcher d2(&pool, &store);

  d1.onTick(makeEvent(evType, "X"));
  d2.onTick(makeEvent(evType, "X"));

  // Each Dispatcher's first event of the type triggers an independent
  // factory call → two fresh instances, no shared state.
  EXPECT_EQ(factoryCalls.load(), 2);
  EXPECT_EQ(invocations.load(), 2);
}
