#pragma once
#include <rapidjson/document.h>
#include <string>
#include <stdexcept>

namespace gma {

class JsonValidator {
public:
    static void validateRequest(const rapidjson::Document& doc);
    static void validateNode(const rapidjson::Value& v);

    // Throw if v[name] is missing or not of expectedType.
    static void requireMember(const rapidjson::Value& v,
                              const char* name,
                              rapidjson::Type expectedType)
    {
        if (!v.HasMember(name) || v[name].GetType() != expectedType) {
            throw std::runtime_error(
                std::string("JSON node missing or wrong-type for field '") +
                name + "'");
        }
    }

    // Validate tree structure recursively (max depth guard).
    static void validateTree(const rapidjson::Value& v, int depth = 0);

private:
    static constexpr int MAX_TREE_DEPTH = 32;
    static constexpr int MAX_ARRAY_SIZE = 64;
};

} // namespace gma
