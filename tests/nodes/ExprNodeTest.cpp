#include "gma/nodes/Expr.hpp"
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

TEST(ExprNodeTest, ScalarInputExposedAsValue) {
  auto sink = std::make_shared<Sink>();
  ExprNode n(compile(R"({"op":"mul","args":[{"ref":"value"},2]})"), sink);

  n.onValue(StreamValue{"SYM", 5.0});
  ASSERT_EQ(sink->received.size(), 1u);
  EXPECT_EQ(sink->received[0].symbol, "SYM");
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 10.0);
}

TEST(ExprNodeTest, RecordFieldsExposedAsRefs) {
  auto sink = std::make_shared<Sink>();
  // (bid + ask) / 2
  ExprNode n(compile(R"({"op":"div","args":[{"op":"add","args":[{"ref":"bid"},{"ref":"ask"}]},2]})"), sink);

  n.onValue(StreamValue{"SYM", recordOf({{"bid", 10.0}, {"ask", 12.0}})});
  ASSERT_EQ(sink->received.size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 11.0);
}

TEST(ExprNodeTest, PredicateEmitsOneOrZero) {
  auto sink = std::make_shared<Sink>();
  ExprNode n(compile(R"({"op":"gt","args":[{"ref":"rsi"},70]})"), sink);

  n.onValue(StreamValue{"S", recordOf({{"rsi", 80.0}})});
  n.onValue(StreamValue{"S", recordOf({{"rsi", 60.0}})});
  ASSERT_EQ(sink->received.size(), 2u);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 1.0);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[1].value), 0.0);
}

TEST(ExprNodeTest, AbsentRefEvaluatesToZero) {
  auto sink = std::make_shared<Sink>();
  ExprNode n(compile(R"({"ref":"nope"})"), sink);
  n.onValue(StreamValue{"S", recordOf({{"present", 1.0}})});
  ASSERT_EQ(sink->received.size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 0.0);
}

TEST(ExprNodeTest, DropsAfterShutdown) {
  auto sink = std::make_shared<Sink>();
  ExprNode n(compile(R"({"ref":"value"})"), sink);
  n.shutdown();
  n.onValue(StreamValue{"S", 1.0});
  EXPECT_TRUE(sink->received.empty());
}
