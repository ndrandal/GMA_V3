#include "gma/nodes/Switch.hpp"
#include "gma/Expr.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <rapidjson/document.h>
#include <vector>

using namespace gma;

namespace {

class Sink : public INode {
public:
  std::vector<StreamValue> received;
  std::atomic<bool> shutdownCalled{false};
  void onValue(const StreamValue& sv) override { received.push_back(sv); }
  void shutdown() noexcept override { shutdownCalled.store(true); }
};

expr::Compiled compile(const char* json) {
  rapidjson::Document d;
  d.Parse(json);
  EXPECT_FALSE(d.HasParseError());
  return expr::compile(d);
}

} // namespace

TEST(SwitchTest, RoutesBySelectorIndex) {
  auto a = std::make_shared<Sink>();
  auto b = std::make_shared<Sink>();
  auto c = std::make_shared<Sink>();
  // selector = the raw scalar value, rounded to an index
  Switch sw(compile(R"({"ref":"value"})"),
            {a, b, c}, /*default=*/nullptr);

  sw.onValue(StreamValue{"S", 0.0});
  sw.onValue(StreamValue{"S", 2.0});
  sw.onValue(StreamValue{"S", 1.0});

  ASSERT_EQ(a->received.size(), 1u);
  ASSERT_EQ(b->received.size(), 1u);
  ASSERT_EQ(c->received.size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(a->received[0].value), 0.0);  // idx 0
  EXPECT_DOUBLE_EQ(std::get<double>(b->received[0].value), 1.0);  // idx 1
  EXPECT_DOUBLE_EQ(std::get<double>(c->received[0].value), 2.0);  // idx 2
}

TEST(SwitchTest, RoundsSelectorToNearestIndex) {
  auto a = std::make_shared<Sink>();
  auto b = std::make_shared<Sink>();
  Switch sw(compile(R"({"ref":"value"})"), {a, b}, nullptr);

  sw.onValue(StreamValue{"S", 0.4});  // -> 0
  sw.onValue(StreamValue{"S", 0.6});  // -> 1
  EXPECT_EQ(a->received.size(), 1u);
  EXPECT_EQ(b->received.size(), 1u);
}

TEST(SwitchTest, OutOfRangeWithoutDefaultDrops) {
  auto a = std::make_shared<Sink>();
  Switch sw(compile(R"({"ref":"value"})"), {a}, nullptr);

  sw.onValue(StreamValue{"S", 5.0});   // out of range
  sw.onValue(StreamValue{"S", -1.0});  // negative
  EXPECT_TRUE(a->received.empty());
}

TEST(SwitchTest, OutOfRangeRoutesToDefault) {
  auto a = std::make_shared<Sink>();
  auto def = std::make_shared<Sink>();
  Switch sw(compile(R"({"ref":"value"})"), {a}, def);

  sw.onValue(StreamValue{"S", 9.0});   // out of range -> default
  ASSERT_EQ(def->received.size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(def->received[0].value), 9.0);
  EXPECT_TRUE(a->received.empty());
}

TEST(SwitchTest, ShutdownPropagatesToBranchesAndDefault) {
  auto a = std::make_shared<Sink>();
  auto b = std::make_shared<Sink>();
  auto def = std::make_shared<Sink>();
  Switch sw(compile(R"({"ref":"value"})"), {a, b}, def);

  EXPECT_EQ(sw.caseCount(), 2u);
  sw.shutdown();
  EXPECT_TRUE(a->shutdownCalled.load());
  EXPECT_TRUE(b->shutdownCalled.load());
  EXPECT_TRUE(def->shutdownCalled.load());

  sw.onValue(StreamValue{"S", 0.0});  // dropped after shutdown
  EXPECT_TRUE(a->received.empty());
}
