#include "gma/Expr.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <set>
#include <string>

using namespace gma;

namespace {

// compile() is eager — the returned closure captures literals/names/funcs by
// value and retains no reference into the Document — so a local Document is safe.
expr::Compiled compileJson(const std::string& json,
                           std::set<std::string>* refs = nullptr) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  EXPECT_FALSE(d.HasParseError()) << json;
  return expr::compile(d, refs);
}

double evalJson(const std::string& json, const expr::Env& env = {}) {
  return compileJson(json)(env);
}

} // namespace

TEST(ExprTest, Literals) {
  EXPECT_DOUBLE_EQ(evalJson("3.5"), 3.5);
  EXPECT_DOUBLE_EQ(evalJson("true"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson("false"), 0.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"lit":7})"), 7.0);
}

TEST(ExprTest, RefPresentAndAbsent) {
  EXPECT_DOUBLE_EQ(evalJson(R"({"ref":"x"})", {{"x", 42.0}}), 42.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"ref":"missing"})", {{"x", 42.0}}), 0.0);
}

TEST(ExprTest, Arithmetic) {
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"add","args":[1,2,3]})"), 6.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"sub","args":[10,4]})"), 6.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"mul","args":[2,3,4]})"), 24.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"div","args":[9,2]})"), 4.5);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"mod","args":[10,3]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"neg","args":[5]})"), -5.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"abs","args":[-5]})"), 5.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"min","args":[3,1,2]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"max","args":[3,1,2]})"), 3.0);
}

TEST(ExprTest, DivAndModByZeroAreFinite) {
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"div","args":[1,0]})"), 0.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"mod","args":[1,0]})"), 0.0);
}

TEST(ExprTest, Comparisons) {
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"gt","args":[2,1]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"gt","args":[1,2]})"), 0.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"lte","args":[2,2]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"eq","args":[3,3]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"neq","args":[3,4]})"), 1.0);
}

TEST(ExprTest, Logical) {
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"and","args":[true,true]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"and","args":[true,false]})"), 0.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"or","args":[false,true]})"), 1.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"not","args":[false]})"), 1.0);
}

TEST(ExprTest, NestedComposition) {
  // a*b + c  with a=2,b=3,c=4 -> 10
  const char* json = R"({
    "op":"add",
    "args":[
      {"op":"mul","args":[{"ref":"a"},{"ref":"b"}]},
      {"ref":"c"}
    ]
  })";
  EXPECT_DOUBLE_EQ(evalJson(json, {{"a", 2.0}, {"b", 3.0}, {"c", 4.0}}), 10.0);
}

TEST(ExprTest, RealWorldFormula) {
  // (bid+ask)/2 - sma  with bid=10, ask=12, sma=9 -> 11 - 9 = 2
  const char* json = R"({
    "op":"sub",
    "args":[
      {"op":"div","args":[{"op":"add","args":[{"ref":"bid"},{"ref":"ask"}]},2]},
      {"ref":"sma"}
    ]
  })";
  EXPECT_DOUBLE_EQ(evalJson(json, {{"bid", 10.0}, {"ask", 12.0}, {"sma", 9.0}}), 2.0);
}

TEST(ExprTest, FnBridgeToFunctionMap) {
  // FunctionMap builtins are installed by the global test bootstrap.
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"fn","name":"sum","args":[1,2,3,4]})"), 10.0);
  EXPECT_DOUBLE_EQ(evalJson(R"({"op":"fn","name":"mean","args":[2,4,6]})"), 4.0);
}

TEST(ExprTest, CollectsReferencedInputs) {
  std::set<std::string> refs;
  auto fn = compileJson(
      R"({"op":"add","args":[{"ref":"bid"},{"op":"mul","args":[{"ref":"ask"},2]}]})",
      &refs);
  EXPECT_EQ(refs, (std::set<std::string>{"bid", "ask"}));
  EXPECT_DOUBLE_EQ(fn({{"bid", 1.0}, {"ask", 3.0}}), 7.0);
}

TEST(ExprTest, MalformedExpressionsThrowAtCompileTime) {
  auto compiles = [](const std::string& json) {
    rapidjson::Document d;
    d.Parse(json.c_str());
    return expr::compile(d);
  };
  EXPECT_THROW(compiles(R"({"op":"bogus","args":[1]})"), std::runtime_error);
  EXPECT_THROW(compiles(R"({"op":"fn","name":"nope","args":[1]})"), std::runtime_error);
  EXPECT_THROW(compiles(R"({"op":"sub","args":[1]})"), std::runtime_error);        // bad arity
  EXPECT_THROW(compiles(R"({"op":"add"})"), std::runtime_error);                   // no args
  EXPECT_THROW(compiles(R"({"ref":5})"), std::runtime_error);                      // ref not string
  EXPECT_THROW(compiles(R"("hello")"), std::runtime_error);                        // bare string
  EXPECT_THROW(compiles(R"({"foo":1})"), std::runtime_error);                      // no ref/lit/op
}
