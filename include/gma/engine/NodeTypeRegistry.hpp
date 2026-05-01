#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <rapidjson/document.h>
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/INode.hpp"

namespace gma::engine {

// Factory signature for pipeline nodes discovered in JSON specs.
// Connectors may register custom node types; engine registers its built-ins.
using NodeBuilderFn = std::function<std::shared_ptr<INode>(
    const rapidjson::Value& spec,
    const std::string&      defaultStreamKey,
    const tree::Deps&       deps,
    std::shared_ptr<INode>  downstream)>;

class NodeTypeRegistry {
public:
  // Tag instance — see EventTypeRegistry::singleton() for rationale.
  static NodeTypeRegistry& singleton() {
    static NodeTypeRegistry s;
    return s;
  }

  static bool registerNodeType(std::string name, NodeBuilderFn fn) {
    std::lock_guard lk(mx());
    auto [it, ok] = map().emplace(std::move(name), std::move(fn));
    return ok;
  }

  static const NodeBuilderFn* find(std::string_view name) {
    std::lock_guard lk(mx());
    auto it = map().find(std::string(name));
    return it == map().end() ? nullptr : &it->second;
  }

  static bool contains(std::string_view name) {
    std::lock_guard lk(mx());
    return map().count(std::string(name)) > 0;
  }

  static std::vector<std::string> names() {
    std::lock_guard lk(mx());
    std::vector<std::string> out;
    out.reserve(map().size());
    for (auto& kv : map()) out.push_back(kv.first);
    return out;
  }

  static void clear() {
    std::lock_guard lk(mx());
    map().clear();
  }

private:
  static std::unordered_map<std::string, NodeBuilderFn>& map() {
    static std::unordered_map<std::string, NodeBuilderFn> m;
    return m;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma::engine
