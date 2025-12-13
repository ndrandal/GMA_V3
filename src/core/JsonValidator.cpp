#include "gma/JsonValidator.hpp"

#include "gma/util/Logger.hpp"

#include <stdexcept>
#include <string>

namespace gma {

void JsonValidator::validateRequest(const rapidjson::Document& doc) {
  if (!doc.IsObject()) {
    throw std::runtime_error("Request must be a JSON object");
  }

  if (!doc.HasMember("id") || !doc["id"].IsString()) {
    throw std::runtime_error("Request missing string id");
  }

  if (!doc.HasMember("tree") || !doc["tree"].IsObject()) {
    throw std::runtime_error("Request missing tree object");
  }

  const std::string id = doc["id"].GetString();

  gma::util::logger().log(
    gma::util::LogLevel::Info,
    "Request validated",
    { {"id", id} }
  );
}

void JsonValidator::validateNode(const rapidjson::Value& v) {
  if (!v.IsObject()) {
    throw std::runtime_error("Node must be an object");
  }

  if (!v.HasMember("type") || !v["type"].IsString()) {
    throw std::runtime_error("Node missing string 'type'");
  }

  const std::string type = v["type"].GetString();

  gma::util::logger().log(
    gma::util::LogLevel::Debug,
    "Node validated",
    { {"type", type} }
  );
}

} // namespace gma
