#pragma once
#include <rapidjson/document.h>
#include <string>
#include <stdexcept>

namespace gma {
class JsonValidator {
public:
    static void validateRequest(const rapidjson::Document& doc);
    static void validateNode(const rapidjson::Value& v);
    // throw if v[name] is missing or not of expectedType
    template<typename T = void>
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
};
}
