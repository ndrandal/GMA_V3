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

// --- validateRequest tests ---

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

TEST(JsonValidatorRequestTest, RejectsEmptyId) {
    auto d = parseDoc("{\"id\":\"\", \"tree\":{}}");
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
    auto d = parseDoc("{\"id\":\"req-1\", \"tree\":{\"type\":\"Worker\", \"fn\":\"sum\"}}");
    EXPECT_NO_THROW(JsonValidator::validateRequest(d));
}

TEST(JsonValidatorRequestTest, AcceptsEmptyTree) {
    // Tree object with no type field — valid (not all tree nodes need type)
    auto d = parseDoc("{\"id\":\"req-2\", \"tree\":{}}");
    EXPECT_NO_THROW(JsonValidator::validateRequest(d));
}

// --- validateNode tests ---

TEST(JsonValidatorNodeTest, RejectsNonObject) {
    auto v = parseDoc("123");
    EXPECT_THROW(JsonValidator::validateNode(v), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsMissingType) {
    auto d = parseDoc("{}");
    EXPECT_THROW(JsonValidator::validateNode(d), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsNonStringType) {
    auto d = parseDoc("{\"type\":123}");
    EXPECT_THROW(JsonValidator::validateNode(d), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsEmptyType) {
    auto d = parseDoc("{\"type\":\"\"}");
    EXPECT_THROW(JsonValidator::validateNode(d), std::runtime_error);
}

TEST(JsonValidatorNodeTest, RejectsUnknownType) {
    auto d = parseDoc("{\"type\":\"Nonexistent\"}");
    EXPECT_THROW(JsonValidator::validateNode(d), std::runtime_error);
}

TEST(JsonValidatorNodeTest, AcceptsKnownTypes) {
    for (const char* t : {"Worker", "Listener", "Aggregate", "Interval",
                          "AtomicAccessor", "SymbolSplit", "Chain"}) {
        std::string json = std::string("{\"type\":\"") + t + "\"}";
        auto d = parseDoc(json);
        EXPECT_NO_THROW(JsonValidator::validateNode(d)) << "Should accept type: " << t;
    }
}

// --- validateTree tests ---

TEST(JsonValidatorTreeTest, RejectsExcessiveDepth) {
    // Build a deeply nested tree
    std::string json = "{\"type\":\"Worker\", \"fn\":\"sum\", \"child\":";
    for (int i = 0; i < 35; ++i) {
        json += "{\"type\":\"Worker\", \"fn\":\"sum\", \"child\":";
    }
    json += "{}";
    for (int i = 0; i < 36; ++i) json += "}";
    auto d = parseDoc(json);
    EXPECT_THROW(JsonValidator::validateTree(d), std::runtime_error);
}

TEST(JsonValidatorTreeTest, AcceptsValidTree) {
    auto d = parseDoc("{\"type\":\"Aggregate\", \"arity\":2, "
                      "\"inputs\":[{\"type\":\"Worker\", \"fn\":\"sum\"}]}");
    EXPECT_NO_THROW(JsonValidator::validateTree(d));
}

// --- requireMember tests ---

TEST(JsonValidatorRequireMemberTest, ThrowsOnMissing) {
    auto d = parseDoc("{\"foo\":1}");
    EXPECT_THROW(JsonValidator::requireMember(d, "bar", kNumberType),
                 std::runtime_error);
}

TEST(JsonValidatorRequireMemberTest, ThrowsOnWrongType) {
    auto d = parseDoc("{\"foo\":\"hello\"}");
    EXPECT_THROW(JsonValidator::requireMember(d, "foo", kNumberType),
                 std::runtime_error);
}

TEST(JsonValidatorRequireMemberTest, PassesOnCorrectType) {
    auto d = parseDoc("{\"foo\":42}");
    EXPECT_NO_THROW(JsonValidator::requireMember(d, "foo", kNumberType));
}
