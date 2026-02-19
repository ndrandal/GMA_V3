#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gma/Span.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include <rapidjson/document.h>

namespace gma {

// Forward declarations of runtime dependencies
class AtomicStore;
class MarketDispatcher;

namespace tree {

  // Dependencies needed to build a tree
  struct Deps {
    AtomicStore*      store      { nullptr };   // for AtomicAccessor
    rt::ThreadPool*       pool       { nullptr };   // for Listener queues
    MarketDispatcher* dispatcher { nullptr };   // for Listener wiring
  };

  // Result of buildForRequest – head plus the downstream chain.
  // `keepAlive` retains every pipeline node to prevent weak_ptr expiry.
  struct BuiltChain {
    std::shared_ptr<INode> head;
    std::vector<std::shared_ptr<INode>> keepAlive;
  };

  // Convenience alias for Worker functions
  using Fn = std::function<ArgType(Span<const ArgType>)>;

  // ---- Low level builders (used internally & for tests) ----

  // Build a single node from JSON spec.
  //  - defaultSymbol: used when spec omits explicit "symbol"
  //  - downstream   : parent node (may be nullptr at the tail)
  std::shared_ptr<INode> buildOne(
    const rapidjson::Value& spec,
    const std::string&      defaultSymbol,
    const Deps&             deps,
    std::shared_ptr<INode>  downstream = nullptr
  );

  // Build a whole tree (root spec is usually the "tree" object from a request)
  std::shared_ptr<INode> buildTree(
    const rapidjson::Value& rootSpec,
    const Deps&             deps
  );

  // ---- Public helpers used by other core components ----

  // Build a pipeline from spec → terminal
  std::shared_ptr<INode> buildNode(
    const rapidjson::Value& spec,
    const std::string&      defaultSymbol,
    const Deps&             deps,
    std::shared_ptr<INode>  terminal
  );

  // Build a simple AtomicAccessor, optionally wrapped in an Interval poller
  std::shared_ptr<INode> buildSimple(
    const std::string&      symbol,
    const std::string&      field,
    int                     pollMs,
    const Deps&             deps,
    std::shared_ptr<INode>  terminal
  );

  // High-level entry: given a **validated request JSON**, build a
  // Listener head wired into the rest of the tree that terminates
  // at `terminal` (usually a Responder).
  BuiltChain buildForRequest(
    const rapidjson::Value& requestJson,
    const Deps&             deps,
    std::shared_ptr<INode>  terminal
  );

} // namespace tree
} // namespace gma
