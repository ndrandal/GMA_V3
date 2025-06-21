#pragma once
#include <rapidjson/document.h>
#include <string>

namespace gma {
class JsonValidator {
public:
    static void validateRequest(const rapidjson::Document& doc);
    static void validateNode(const rapidjson::Value& v);
};
}
