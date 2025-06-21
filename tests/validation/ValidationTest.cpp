#include "gma/JsonValidator.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <string>

using namespace gma;
using namespace rapidjson;

// Helper: parse JSON string into Document
static Document parseDoc(const std::string& s) {
    Document d;
    d.Parse(s.c_str());
    EXPECT_FALSE(d.HasParseError()) << "Failed to parse JSON: " << s;
    return d;
}

TEST(JsonValidatorRequestTest, RejectsNonObject) {
    auto d = parseDoc("[1,2,3]");
    EXPECT_THROW(JsonValidator::validateRequest(d), std::runtime_error);
}

TEST(JsonValidatorRequestTest, RejectsMissingId) {
    auto d = parseDoc("{\"tree\":{}}");
    EXPECT_THROW(JsonValidator::validateRequest(d), std::runtime_error);
}

TEST(JsonValidatorRequestTest, RejectsNonStringId) {
    auto d = parseDoc("{\"id\":123, \"tree\":{}}");
    EXPECT_THROW(JsonValidator::validateRequest(d), std::runtime_error);
}

TEST(JsonValidatorRequestTest, RejectsMissingTree) {
    auto d = parseDoc("{\"id\":\"1\"}");
    EXPECT_THROW(JsonValidator::validateRequest(d), std::runtime_error);
}

TEST(JsonValidatorRequestTest, RejectsNonObjectTree) {
    auto d = parseDoc("{\"id\":\"1\", \"tree\":123}");
    EXPECT_THROW(JsonValidator::validateRequest(d), std::runtime_error);
}

TEST(JsonValidatorRequestTest, AcceptsValidRequest) {
    auto d = parseDoc("{\"id\":\"1\", \"tree\":{\"type\":\"X\"}}");
    EXPECT_NO_THROW(JsonValidator::validateRequest(d));
}

TEST(JsonValidatorNodeTest, RejectsNonObject) {
    auto v = parseDoc("123");
    EXPECT_THROW(JsonValidator::validateNode(v), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsMissingType) {
    auto d = parseDoc("{}" );
    auto& v = d;
    EXPECT_THROW(JsonValidator::validateNode(v), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsNonStringType) {
    auto d = parseDoc("{\"type\":123}");
    EXPECT_THROW(JsonValidator::validateNode(d), std::runtime_error);
}

TEST(JsonValidatorNodeTest, AcceptsValidNode) {
    auto d = parseDoc("{\"type\":\"SomeType\", \"extra\":true}");
    EXPECT_NO_THROW(JsonValidator::validateNode(d));
}
