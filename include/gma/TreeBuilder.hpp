#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <span>
#include <stdexcept>

// IMPORTANT: never forward-declare rapidjson::Value — include the header.
#include <rapidjson/document.h>

namespace gma {

// Simplify first: a scalar evaluation type.
// If you need richer payloads later, switch to std::variant<>.
using ArgType = double;

struct INode {
  virtual ~INode() = default;

  // Evaluate node; by default we just evaluate children and feed to our op.
  // If your graph has external inputs, you can extend this signature later.
  virtual ArgType eval() = 0;
};

namespace tree {

// Function signature each op implements: f(children_values...)
using Span      = std::span<const ArgType>;
using OpFunc    = std::function<ArgType(Span)>;

// Dependency bundle the builder needs (op registry etc.)
struct Deps {
  std::unordered_map<std::string, OpFunc> ops;
};

// Build a full tree from a JSON object spec { "op": "...", "args": [...] }.
// Returns the root node.
std::shared_ptr<INode> buildTree(const rapidjson::Value& spec, const Deps& deps);

// Build a single node, optionally attaching to a parent (parent is not used by
// this minimal implementation; kept to satisfy your existing signature).
std::shared_ptr<INode> buildOne(const rapidjson::Value& spec,
                                const std::string&      name,
                                const Deps&             deps,
                                std::shared_ptr<INode>  parent = nullptr);

// Provide a default op registry (add, sub, mul, div, min, max, const, sum, mean).
Deps defaultDeps();

} // namespace tree
} // namespace gma
