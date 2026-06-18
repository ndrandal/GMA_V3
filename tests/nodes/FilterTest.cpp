#include "gma/nodes/Filter.hpp"
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
  void onValue(const StreamValue& sv) override { received.push_back(sv); }
  void shutdown() noexcept override {}
};

expr::Compiled compile(const char* json) {
  rapidjson::Document d;
  d.Parse(json);
  EXPECT_FALSE(d.HasParseError());
  return expr::compile(d);
}

ArgType recordOf(std::initializer_list<std::pair<const char*, double>> kvs) {
  Record r;
  for (auto& kv : kvs) recordSet(r, kv.first, kv.second);
  return ArgType{r};
}

} // namespace

TEST(FilterTest, ForwardsWhenPredicateTrueDropsWhenFalse) {
  auto sink = std::make_shared<Sink>();
  Filter f(compile(R"({"op":"gt","args":[{"ref":"value"},0]})"), sink);

  f.onValue(StreamValue{"S", 5.0});    // 5 > 0 -> pass
  f.onValue(StreamValue{"S", -2.0});   // -2 > 0 -> drop
  f.onValue(StreamValue{"S", 3.0});    // pass

  ASSERT_EQ(sink->received.size(), 2u);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 5.0);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[1].value), 3.0);
}

TEST(FilterTest, ForwardsValueUnchanged) {
  auto sink = std::make_shared<Sink>();
  // gate a record on rsi > 70, forwarding the whole record
  Filter f(compile(R"({"op":"gt","args":[{"ref":"rsi"},70]})"), sink);

  f.onValue(StreamValue{"S", recordOf({{"rsi", 80.0}, {"price", 12.0}})});
  ASSERT_EQ(sink->received.size(), 1u);
  ASSERT_TRUE(std::holds_alternative<Record>(sink->received[0].value));
  const Record& r = std::get<Record>(sink->received[0].value);
  EXPECT_DOUBLE_EQ(std::get<double>(*recordFind(r, "price")), 12.0);
}

TEST(FilterTest, RecordFailingPredicateIsDropped) {
  auto sink = std::make_shared<Sink>();
  Filter f(compile(R"({"op":"gt","args":[{"ref":"rsi"},70]})"), sink);
  f.onValue(StreamValue{"S", recordOf({{"rsi", 50.0}})});
  EXPECT_TRUE(sink->received.empty());
}

TEST(FilterTest, DropsAfterShutdown) {
  auto sink = std::make_shared<Sink>();
  Filter f(compile("true"), sink);  // always-pass predicate
  f.shutdown();
  f.onValue(StreamValue{"S", 1.0});
  EXPECT_TRUE(sink->received.empty());
}
