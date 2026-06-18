#include "gma/FunctionMap.hpp"
#include "gma/Expr.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

using namespace gma;

// FunctionMap builtins are installed by the global test bootstrap.

TEST(SelectFunctionTest, SelectsByCondition) {
  auto fn = FunctionMap::instance().getFunction("select");
  EXPECT_DOUBLE_EQ(fn({1.0, 10.0, 20.0}), 10.0);  // cond truthy -> a
  EXPECT_DOUBLE_EQ(fn({0.0, 10.0, 20.0}), 20.0);  // cond falsy  -> b
  EXPECT_DOUBLE_EQ(fn({0.6, 10.0, 20.0}), 10.0);  // >0.5 is truthy
  EXPECT_DOUBLE_EQ(fn({0.4, 10.0, 20.0}), 20.0);
}

TEST(SelectFunctionTest, TooFewInputsYieldsZero) {
  auto fn = FunctionMap::instance().getFunction("select");
  EXPECT_DOUBLE_EQ(fn({1.0}), 0.0);
  EXPECT_DOUBLE_EQ(fn({1.0, 5.0}), 0.0);
}

TEST(SelectFunctionTest, IffIsAlias) {
  auto fn = FunctionMap::instance().getFunction("iff");
  EXPECT_DOUBLE_EQ(fn({1.0, 5.0, 9.0}), 5.0);
  EXPECT_DOUBLE_EQ(fn({0.0, 5.0, 9.0}), 9.0);
}

TEST(SelectFunctionTest, UsableAsTernaryInsideExpr) {
  // if rsi > 70 then 1 else 0
  rapidjson::Document d;
  d.Parse(R"({"op":"fn","name":"select","args":[
    {"op":"gt","args":[{"ref":"rsi"},70]}, 1, 0
  ]})");
  ASSERT_FALSE(d.HasParseError());
  auto fn = expr::compile(d);
  EXPECT_DOUBLE_EQ(fn({{"rsi", 80.0}}), 1.0);
  EXPECT_DOUBLE_EQ(fn({{"rsi", 60.0}}), 0.0);
}
