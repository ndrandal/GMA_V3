#pragma once
#include <rapidjson/document.h>
#include <string>
#include <stdexcept>

namespace gma {

class JsonValidator {
public:
    static void validateRequest(const rapidjson::Document& doc);
    static void validateNode(const rapidjson::Value& v);

    // Throw if v[name] is missing or not of expectedType. Booleans are a
    // special case: RapidJSON tags true/false with the distinct kTrueType /
    // kFalseType, so an exact GetType() compare against either would reject the
    // other. When a bool is expected (caller passes kTrueType OR kFalseType),
    // accept any boolean value (ENC-804/L1).
    static void requireMember(const rapidjson::Value& v,
                              const char* name,
                              rapidjson::Type expectedType)
    {
        if (!v.HasMember(name)) {
            throw std::runtime_error(
                std::string("JSON node missing or wrong-type for field '") +
                name + "'");
        }
        const rapidjson::Value& m = v[name];
        const bool wantBool = (expectedType == rapidjson::kTrueType ||
                               expectedType == rapidjson::kFalseType);
        const bool ok = wantBool ? m.IsBool() : (m.GetType() == expectedType);
        if (!ok) {
            throw std::runtime_error(
                std::string("JSON node missing or wrong-type for field '") +
                name + "'");
        }
    }

    // Validate tree structure recursively (max depth guard).
    static void validateTree(const rapidjson::Value& v, int depth = 0);

private:
    static constexpr int    MAX_TREE_DEPTH  = 32;
    static constexpr int    MAX_ARRAY_SIZE  = 64;
    static constexpr size_t MAX_STRING_LEN  = 4096;
};

} // namespace gma
