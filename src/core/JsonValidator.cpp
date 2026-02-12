#include "gma/JsonValidator.hpp"
#include "gma/util/Logger.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace gma {

static const std::unordered_set<std::string> KNOWN_NODE_TYPES = {
    "Listener", "Worker", "Aggregate", "Interval",
    "AtomicAccessor", "SymbolSplit", "Chain"
};

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

  if (KNOWN_NODE_TYPES.find(type) == KNOWN_NODE_TYPES.end()) {
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

  // If this node has a "type" field, validate it as a node
  if (v.HasMember("type")) {
    validateNode(v);
  }

  // Recurse into child nodes
  if (v.HasMember("child") && v["child"].IsObject()) {
    validateTree(v["child"], depth + 1);
  }

  // Recurse into inputs array
  if (v.HasMember("inputs") && v["inputs"].IsArray()) {
    const auto& arr = v["inputs"];
    if (static_cast<int>(arr.Size()) > MAX_ARRAY_SIZE) {
      throw std::runtime_error("'inputs' array exceeds maximum size of " +
                               std::to_string(MAX_ARRAY_SIZE));
    }
    for (const auto& elem : arr.GetArray()) {
      if (elem.IsObject()) {
        validateTree(elem, depth + 1);
      }
    }
  }

  // Recurse into stages/pipeline arrays
  const char* arrayKeys[] = {"stages", "pipeline"};
  for (const char* key : arrayKeys) {
    if (v.HasMember(key) && v[key].IsArray()) {
      const auto& arr = v[key];
      if (static_cast<int>(arr.Size()) > MAX_ARRAY_SIZE) {
        throw std::runtime_error(std::string("'") + key +
                                 "' array exceeds maximum size of " +
                                 std::to_string(MAX_ARRAY_SIZE));
      }
      for (const auto& elem : arr.GetArray()) {
        if (elem.IsObject()) {
          validateTree(elem, depth + 1);
        }
      }
    }
  }

  // Recurse into "node" field
  if (v.HasMember("node") && v["node"].IsObject()) {
    validateTree(v["node"], depth + 1);
  }
}

} // namespace gma
