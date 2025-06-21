#include "gma/JsonValidator.hpp"
#include "gma/Logger.hpp"
using namespace gma;

void JsonValidator::validateRequest(const rapidjson::Document& doc) {
    if (!doc.IsObject()) throw std::runtime_error("Request must be a JSON object");
    if (!doc.HasMember("id") || !doc["id"].IsString())
        throw std::runtime_error("Request missing string id");
    if (!doc.HasMember("tree") || !doc["tree"].IsObject())
        throw std::runtime_error("Request missing tree object");
    Logger::info("Request validated: id=" + std::string(doc["id"].GetString()));
}

void JsonValidator::validateNode(const rapidjson::Value& v) {
    if (!v.IsObject())
        throw std::runtime_error("Node must be an object");
    if (!v.HasMember("type") || !v["type"].IsString())
        throw std::runtime_error("Node missing string 'type'");
    Logger::info("Node validated: type=" + std::string(v["type"].GetString()));
}
