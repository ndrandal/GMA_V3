#include "gma/JsonValidator.hpp"
#include "gma/engine/NodeTypeRegistry.hpp"
#include "gma/util/Logger.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace gma {

void JsonValidator::validateRequest(const rapidjson::Document& doc) {
  if (!doc.IsObject()) {
    throw std::runtime_error("Request must be a JSON object");
  }

  if (!doc.HasMember("id") || !doc["id"].IsString()) {
    throw std::runtime_error("Request missing string 'id'");
  }

  const std::string id = doc["id"].GetString();
  if (id.empty()) {
    throw std::runtime_error("Request 'id' must not be empty");
  }
  if (id.size() > MAX_STRING_LEN) {
    throw std::runtime_error("field 'id' exceeds maximum length");
  }

  if (!doc.HasMember("tree") || !doc["tree"].IsObject()) {
    throw std::runtime_error("Request missing 'tree' object");
  }

  // Recursively validate the tree structure
  validateTree(doc["tree"]);

  gma::util::logger().log(
    gma::util::LogLevel::Debug,
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
  if (type.empty()) {
    throw std::runtime_error("Node 'type' must not be empty");
  }
  if (type.size() > MAX_STRING_LEN) {
    throw std::runtime_error("field 'type' exceeds maximum length");
  }

  if (!engine::NodeTypeRegistry::contains(type)) {
    throw std::runtime_error("Unknown node type: '" + type + "'");
  }
}

void JsonValidator::validateTree(const rapidjson::Value& v, int depth) {
  if (depth > MAX_TREE_DEPTH) {
    throw std::runtime_error("Tree exceeds maximum depth of " +
                             std::to_string(MAX_TREE_DEPTH));
  }

  if (!v.IsObject()) {
    throw std::runtime_error("Tree node must be an object");
  }

  // Open-vocabulary walk: every member is checked / recursed into based on
  // its value's type, not its key name. This means connector-introduced
  // node types with new sub-spec keys get the same length / depth /
  // array-size checks as engine built-ins, no allowlist edits required.
  for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
    const char* key = it->name.GetString();
    const auto& val = it->value;

    if (val.IsString()) {
      if (std::strlen(val.GetString()) > MAX_STRING_LEN) {
        throw std::runtime_error(
            std::string("field '") + key + "' exceeds maximum length");
      }
    } else if (val.IsObject()) {
      validateTree(val, depth + 1);
    } else if (val.IsArray()) {
      if (static_cast<int>(val.Size()) > MAX_ARRAY_SIZE) {
        throw std::runtime_error(std::string("'") + key +
                                 "' array exceeds maximum size of " +
                                 std::to_string(MAX_ARRAY_SIZE));
      }
      for (const auto& elem : val.GetArray()) {
        if (elem.IsString()) {
          if (std::strlen(elem.GetString()) > MAX_STRING_LEN) {
            throw std::runtime_error(
                std::string("element in array '") + key +
                "' exceeds maximum length");
          }
        } else if (elem.IsObject()) {
          validateTree(elem, depth + 1);
        }
      }
    }
  }

  // If this node has a "type" field, look it up in NodeTypeRegistry.
  // (Length already checked above; here we only verify the type name
  // resolves to a registered builder.)
  if (v.HasMember("type")) {
    validateNode(v);
  }
}

} // namespace gma
