#include "gma/engine/ConfigNamespaceRegistry.hpp"
#include "gma/engine/EventComputerRegistry.hpp"
#include "gma/engine/EventTypeRegistry.hpp"
#include "gma/engine/IEventComputer.hpp"
#include "gma/engine/IngressRegistry.hpp"
#include "gma/engine/NodeTypeRegistry.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/Event.hpp"

#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace gma;
using namespace gma::engine;

namespace {

class NoopNode final : public INode {
public:
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override {}
};

class NoopComputer final : public IEventComputer {
public:
  explicit NoopComputer(std::string type, int* sink = nullptr)
    : type_(std::move(type)), sink_(sink) {}
  std::string_view eventType() const override { return type_; }
  void compute(const Event&, ComputeContext&) override { if (sink_) ++*sink_; }
private:
  std::string type_;
  int*        sink_;
};

class NoopIngress final : public IIngressSource {
public:
  void start() override {}
  void stop() noexcept override {}
};

} // namespace

// ---------- EventTypeRegistry ----------

TEST(EventTypeRegistryTest, RegisterFindContains) {
  EventTypeRegistry::clear();
  EXPECT_TRUE(EventTypeRegistry::registerEvent({"tick", {"lastPrice", "volume"}, true}));
  EXPECT_TRUE(EventTypeRegistry::contains("tick"));

  const auto* schema = EventTypeRegistry::find("tick");
  ASSERT_NE(schema, nullptr);
  EXPECT_EQ(schema->name, "tick");
  ASSERT_EQ(schema->knownFields.size(), 2u);
  EXPECT_EQ(schema->knownFields[0], "lastPrice");
  EXPECT_TRUE(schema->dispatchable);
}

TEST(EventTypeRegistryTest, DuplicateRejected) {
  EventTypeRegistry::clear();
  EXPECT_TRUE(EventTypeRegistry::registerEvent({"tick", {}, true}));
  EXPECT_FALSE(EventTypeRegistry::registerEvent({"tick", {"newFields"}, true}));

  // Original entry unchanged
  const auto* schema = EventTypeRegistry::find("tick");
  ASSERT_NE(schema, nullptr);
  EXPECT_TRUE(schema->knownFields.empty());
}

TEST(EventTypeRegistryTest, MissingFindReturnsNull) {
  EventTypeRegistry::clear();
  EXPECT_EQ(EventTypeRegistry::find("nope"), nullptr);
  EXPECT_FALSE(EventTypeRegistry::contains("nope"));
}

TEST(EventTypeRegistryTest, NamesListsAll) {
  EventTypeRegistry::clear();
  EventTypeRegistry::registerEvent({"a", {}, true});
  EventTypeRegistry::registerEvent({"b", {}, true});
  auto names = EventTypeRegistry::names();
  EXPECT_EQ(names.size(), 2u);
}

// ---------- EventComputerRegistry ----------
// Tests use unique event-type names so they don't interfere with any factories
// registered at bootstrap (e.g. the market "tick" computer once Step 8 lands).

TEST(EventComputerRegistryTest, RegisterAndFactoryCount) {
  const std::string t = "__test_cr_type__";
  EventComputerRegistry::registerFactory(t,
    [] { return std::make_unique<NoopComputer>("__test_cr_type__"); });
  EventComputerRegistry::registerFactory(t,
    [] { return std::make_unique<NoopComputer>("__test_cr_type__"); });
  EXPECT_EQ(EventComputerRegistry::factoryCount(t), 2u);
  EXPECT_EQ(EventComputerRegistry::factoryCount("__test_cr_absent__"), 0u);
}

TEST(EventComputerRegistryTest, CreateAllYieldsFreshInstancesInOrder) {
  const std::string t = "__test_cr_order__";
  int sinkA = 0, sinkB = 0;
  EventComputerRegistry::registerFactory(t,
    [&] { return std::make_unique<NoopComputer>("__test_cr_order__", &sinkA); });
  EventComputerRegistry::registerFactory(t,
    [&] { return std::make_unique<NoopComputer>("__test_cr_order__", &sinkB); });

  auto batch1 = EventComputerRegistry::createAll(t);
  ASSERT_EQ(batch1.size(), 2u);
  Event ev;
  ComputeContext ctx{};
  batch1[0]->compute(ev, ctx);
  batch1[1]->compute(ev, ctx);
  EXPECT_EQ(sinkA, 1);
  EXPECT_EQ(sinkB, 1);

  // A second createAll produces fresh instances (not shared with batch1).
  auto batch2 = EventComputerRegistry::createAll(t);
  ASSERT_EQ(batch2.size(), 2u);
  EXPECT_NE(batch2[0].get(), batch1[0].get());
}

TEST(EventComputerRegistryTest, CreateAllOnMissingTypeReturnsEmpty) {
  auto out = EventComputerRegistry::createAll("__test_cr_missing__");
  EXPECT_TRUE(out.empty());
}

TEST(EventComputerRegistryTest, NullFactoryIgnored) {
  const std::string t = "__test_cr_null__";
  EventComputerRegistry::registerFactory(t, EventComputerRegistry::Factory{});
  EXPECT_EQ(EventComputerRegistry::factoryCount(t), 0u);
}

// ---------- NodeTypeRegistry ----------
// NodeTypeRegistry is populated at test-binary bootstrap with engine builtins
// (Listener, Worker, …). These tests must NOT call clear() — doing so would
// wipe the builtins and break every subsequent test that builds a tree. Use
// unique names to avoid collision.

TEST(NodeTypeRegistryTest, RegisterAndFind) {
  auto builder = [](const rapidjson::Value&, const std::string&,
                    const tree::Deps&, std::shared_ptr<INode>) -> std::shared_ptr<INode> {
    return std::make_shared<NoopNode>();
  };
  EXPECT_TRUE(NodeTypeRegistry::registerNodeType("__test_noop_find__", builder));
  EXPECT_TRUE(NodeTypeRegistry::contains("__test_noop_find__"));

  const auto* fn = NodeTypeRegistry::find("__test_noop_find__");
  ASSERT_NE(fn, nullptr);

  rapidjson::Document doc;
  auto node = (*fn)(doc, "AAPL", {}, nullptr);
  ASSERT_NE(node, nullptr);
}

TEST(NodeTypeRegistryTest, DuplicateRejected) {
  auto builder = [](const rapidjson::Value&, const std::string&,
                    const tree::Deps&, std::shared_ptr<INode>) -> std::shared_ptr<INode> {
    return nullptr;
  };
  EXPECT_TRUE(NodeTypeRegistry::registerNodeType("__test_dup__", builder));
  EXPECT_FALSE(NodeTypeRegistry::registerNodeType("__test_dup__", builder));
}

TEST(NodeTypeRegistryTest, MissingFindReturnsNull) {
  EXPECT_EQ(NodeTypeRegistry::find("__test_absent__"), nullptr);
  EXPECT_FALSE(NodeTypeRegistry::contains("__test_absent__"));
}

// ---------- IngressRegistry ----------

TEST(IngressRegistryTest, RegisterAndFind) {
  IngressRegistry::clear();
  auto factory = [](EngineRegistries&) -> std::unique_ptr<IIngressSource> {
    return std::make_unique<NoopIngress>();
  };
  EXPECT_TRUE(IngressRegistry::registerIngress("synthetic", factory));
  EXPECT_TRUE(IngressRegistry::contains("synthetic"));
  EXPECT_NE(IngressRegistry::find("synthetic"), nullptr);
}

TEST(IngressRegistryTest, DuplicateRejected) {
  IngressRegistry::clear();
  auto factory = [](EngineRegistries&) -> std::unique_ptr<IIngressSource> {
    return nullptr;
  };
  EXPECT_TRUE(IngressRegistry::registerIngress("x", factory));
  EXPECT_FALSE(IngressRegistry::registerIngress("x", factory));
}

// ---------- ConfigNamespaceRegistry ----------

TEST(ConfigNamespaceRegistryTest, DispatchRoutesByPrefix) {
  ConfigNamespaceRegistry::clear();
  std::string lastKey, lastValue;
  ConfigNamespaceRegistry::registerNamespace(
      "market",
      [&](std::string_view k, std::string_view v) {
        lastKey.assign(k);
        lastValue.assign(v);
        return true;
      });

  EXPECT_TRUE(ConfigNamespaceRegistry::dispatch("market.taSMA", "5,10,20"));
  EXPECT_EQ(lastKey, "taSMA");
  EXPECT_EQ(lastValue, "5,10,20");
}

TEST(ConfigNamespaceRegistryTest, UnknownPrefixReturnsFalse) {
  ConfigNamespaceRegistry::clear();
  EXPECT_FALSE(ConfigNamespaceRegistry::dispatch("nobody.x", "y"));
}

TEST(ConfigNamespaceRegistryTest, KeyWithoutDotIsIgnored) {
  ConfigNamespaceRegistry::clear();
  ConfigNamespaceRegistry::registerNamespace("m", [](auto, auto) { return true; });
  EXPECT_FALSE(ConfigNamespaceRegistry::dispatch("bareKey", "v"));
}

TEST(ConfigNamespaceRegistryTest, ReaderReturnFalseIsPropagated) {
  ConfigNamespaceRegistry::clear();
  ConfigNamespaceRegistry::registerNamespace(
      "m", [](std::string_view, std::string_view) { return false; });
  EXPECT_FALSE(ConfigNamespaceRegistry::dispatch("m.key", "v"));
}

TEST(ConfigNamespaceRegistryTest, DuplicatePrefixRejected) {
  ConfigNamespaceRegistry::clear();
  EXPECT_TRUE(ConfigNamespaceRegistry::registerNamespace("p", [](auto, auto) { return true; }));
  EXPECT_FALSE(ConfigNamespaceRegistry::registerNamespace("p", [](auto, auto) { return true; }));
}
