#pragma once

#include <memory>
#include <rapidjson/document.h>

namespace gma {
  class ExecutionContext;
  class MarketDispatcher;
  class INode;

  /// Builds a processing tree of INode instances from JSON.
  class TreeBuilder {
  public:
    /// \param json       The full request JSON (must contain a "tree" object).
    /// \param ctx        Supplies thread‑pool and store.
    /// \param dispatcher Where to register Listener nodes.
    static std::shared_ptr<INode> build(const rapidjson::Document& json,
                                        ExecutionContext* ctx,
                                        MarketDispatcher* dispatcher);

  private:
    /// Recursively builds nodes, chaining \p downstream at the tail.
    static std::shared_ptr<INode> buildInternal(const rapidjson::Value&   nodeJson,
                                                 ExecutionContext*          ctx,
                                                 MarketDispatcher*         dispatcher,
                                                 std::shared_ptr<INode>    downstream);
  };
}
