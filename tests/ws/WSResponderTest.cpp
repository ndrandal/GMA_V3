#include "gma/ws/WSResponder.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>
#include <vector>

using namespace gma;
using namespace gma::ws;

// Helper: capture the JSON string sent by WSResponder
static std::string captureOnValue(const SymbolValue& sv) {
    std::string captured;
    WSResponder resp("req-1", [&](const std::string& json) { captured = json; });
    resp.onValue(sv);
    return captured;
}

static rapidjson::Document parseJson(const std::string& s) {
    rapidjson::Document d;
    d.Parse(s.c_str());
    return d;
}

TEST(WSResponderTest, OnValueDouble) {
    auto json = captureOnValue(SymbolValue{"AAPL", 150.5});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    EXPECT_STREQ(d["type"].GetString(), "update");
    EXPECT_STREQ(d["id"].GetString(), "req-1");
    EXPECT_STREQ(d["symbol"].GetString(), "AAPL");
    EXPECT_DOUBLE_EQ(d["value"].GetDouble(), 150.5);
}

TEST(WSResponderTest, OnValueInt) {
    auto json = captureOnValue(SymbolValue{"SYM", ArgType(42)});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    EXPECT_EQ(d["value"].GetInt(), 42);
}

TEST(WSResponderTest, OnValueBool) {
    auto json = captureOnValue(SymbolValue{"SYM", ArgType(true)});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    EXPECT_TRUE(d["value"].GetBool());
}

TEST(WSResponderTest, OnValueString) {
    auto json = captureOnValue(SymbolValue{"SYM", ArgType(std::string("hello"))});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    EXPECT_STREQ(d["value"].GetString(), "hello");
}

TEST(WSResponderTest, OnValueVectorInt) {
    std::vector<int> v = {1, 2, 3};
    auto json = captureOnValue(SymbolValue{"SYM", ArgType(v)});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    ASSERT_TRUE(d["value"].IsArray());
    auto arr = d["value"].GetArray();
    ASSERT_EQ(arr.Size(), 3u);
    EXPECT_EQ(arr[0].GetInt(), 1);
    EXPECT_EQ(arr[1].GetInt(), 2);
    EXPECT_EQ(arr[2].GetInt(), 3);
}

TEST(WSResponderTest, OnValueVectorDouble) {
    std::vector<double> v = {1.5, 2.5};
    auto json = captureOnValue(SymbolValue{"SYM", ArgType(v)});
    auto d = parseJson(json);
    ASSERT_FALSE(d.HasParseError());
    ASSERT_TRUE(d["value"].IsArray());
    auto arr = d["value"].GetArray();
    ASSERT_EQ(arr.Size(), 2u);
    EXPECT_DOUBLE_EQ(arr[0].GetDouble(), 1.5);
    EXPECT_DOUBLE_EQ(arr[1].GetDouble(), 2.5);
}

TEST(WSResponderTest, NullSendNoCrash) {
    WSResponder resp("req-x", nullptr);
    EXPECT_NO_THROW(resp.onValue(SymbolValue{"SYM", 1.0}));
}

TEST(WSResponderTest, ShutdownIsNoOp) {
    WSResponder resp("req-x", [](const std::string&){});
    EXPECT_NO_THROW(resp.shutdown());
}
