#include "gma/StreamValue.hpp"
#include "gma/util/JsonUtil.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <string>
#include <vector>

using namespace gma;

namespace {

ArgType roundTrip(const std::string& json) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  EXPECT_FALSE(d.HasParseError());
  return util::readArgTypeJson(d);
}

} // namespace

TEST(RecordValueTest, SetGetAndAbsentField) {
  Record r;
  recordSet(r, "o", 1.5);
  recordSet(r, "c", 2.5);

  const ArgType* o = recordFind(r, "o");
  ASSERT_NE(o, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*o), 1.5);

  const ArgType* c = recordFind(r, "c");
  ASSERT_NE(c, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*c), 2.5);

  EXPECT_EQ(recordFind(r, "missing"), nullptr);
}

TEST(RecordValueTest, OverwriteKeepsPositionAndUpdatesValue) {
  Record r;
  recordSet(r, "a", 1.0);
  recordSet(r, "b", 2.0);
  recordSet(r, "a", 9.0); // overwrite — must stay first

  ASSERT_EQ(r.fields.size(), 2u);
  EXPECT_EQ(r.fields[0].name, "a");
  EXPECT_DOUBLE_EQ(std::get<double>(r.fields[0].value.value), 9.0);
  EXPECT_EQ(r.fields[1].name, "b");
}

TEST(RecordValueTest, SerializesToJsonObjectInInsertionOrder) {
  Record r;
  recordSet(r, "o", 1.5);
  recordSet(r, "h", 3.5);
  recordSet(r, "c", 2.5);

  EXPECT_EQ(util::toJsonString(ArgType{r}),
            R"({"o":1.5,"h":3.5,"c":2.5})");
}

TEST(RecordValueTest, NestedRecordSerializes) {
  Record inner;
  recordSet(inner, "b", 2.5);

  Record outer;
  recordSet(outer, "a", ArgType{inner});

  EXPECT_EQ(util::toJsonString(ArgType{outer}),
            R"({"a":{"b":2.5}})");
}

TEST(RecordValueTest, ReadBuildsRecordFromJsonObject) {
  ArgType v = roundTrip(R"({"o":1.5,"c":2.5})");
  ASSERT_TRUE(std::holds_alternative<Record>(v));
  const Record& r = std::get<Record>(v);
  ASSERT_EQ(r.fields.size(), 2u);
  EXPECT_EQ(r.fields[0].name, "o");
  EXPECT_DOUBLE_EQ(std::get<double>(r.fields[0].value.value), 1.5);
}

TEST(RecordValueTest, JsonRoundTripPreservesStructure) {
  // Flat record of doubles.
  const std::string flat = R"({"o":1.5,"h":3.5,"l":1.0,"c":2.5})";
  EXPECT_EQ(util::toJsonString(roundTrip(flat)), flat);

  // Nested object + heterogeneous array.
  const std::string nested = R"({"a":{"b":2.5},"arr":[1.0,{"k":3.0}]})";
  EXPECT_EQ(util::toJsonString(roundTrip(nested)), nested);

  // Scalars and bool.
  EXPECT_EQ(util::toJsonString(roundTrip(R"({"flag":true,"n":2.0})")),
            R"({"flag":true,"n":2.0})");
}

TEST(RecordValueTest, RecordCarriedOnStreamValue) {
  Record r;
  recordSet(r, "price", 10.0);
  StreamValue sv{"AAPL", ArgType{r}};

  ASSERT_TRUE(std::holds_alternative<Record>(sv.value));
  const ArgType* p = recordFind(std::get<Record>(sv.value), "price");
  ASSERT_NE(p, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*p), 10.0);
}
