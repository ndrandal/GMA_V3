#include "gma/StreamValue.hpp"
#include "gma/util/JsonUtil.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <string>
#include <type_traits>
#include <utility>
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

// ----- ENC-1073 regression guards -----
//
// Record's special members are declared in StreamValue.hpp and defaulted below
// RecordField, so that naming Record as a std::variant alternative does not
// instantiate std::vector<RecordField>'s members while RecordField is still
// incomplete. That restructuring is what makes the header compile under clang.
//
// These asserts pin the observable consequences. They are the part a future
// "simplify this back to implicit members" edit would break in a way g++ alone
// would not report: the noexcept moves in particular are what let containers of
// Record relocate by move instead of copy.
static_assert(std::is_default_constructible_v<Record>);
static_assert(std::is_copy_constructible_v<Record>);
static_assert(std::is_copy_assignable_v<Record>);
static_assert(std::is_nothrow_move_constructible_v<Record>);
static_assert(std::is_nothrow_move_assignable_v<Record>);
static_assert(std::is_nothrow_destructible_v<Record>);
static_assert(std::is_nothrow_move_constructible_v<ArgType>);

TEST(RecordValueTest, CopyIsDeepAndMovePreservesFields) {
  Record inner;
  recordSet(inner, "n", 1.0);
  Record r;
  recordSet(r, "a", 1.0);
  recordSet(r, "nested", inner);

  // Copy is independent of the original, all the way through the nesting.
  Record copy = r;
  recordSet(r, "a", 99.0);
  recordSet(inner, "n", 99.0);
  const ArgType* copiedA = recordFind(copy, "a");
  ASSERT_NE(copiedA, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*copiedA), 1.0);

  const ArgType* copiedNested = recordFind(copy, "nested");
  ASSERT_NE(copiedNested, nullptr);
  const ArgType* copiedN = recordFind(std::get<Record>(*copiedNested), "n");
  ASSERT_NE(copiedN, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*copiedN), 1.0);

  // Move carries the fields across intact.
  Record moved = std::move(copy);
  ASSERT_EQ(moved.fields.size(), 2u);
  const ArgType* movedA = recordFind(moved, "a");
  ASSERT_NE(movedA, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*movedA), 1.0);
}
