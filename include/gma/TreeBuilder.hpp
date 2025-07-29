#pragma once

#include <memory>
#include <rapidjson/document.h>
#include "gma/FunctionMap.hpp"

namespace gma {
  class ExecutionContext;
  class MarketDispatcher;
  class INode;
  class AtomicStore;

  namespace nodes { class AtomicAccessor; class Worker; class Aggregate; class SymbolSplit; class Interval; }

  /// Builds a processing tree of INode instances from JSON.
  class TreeBuilder {
  public:
    /// \param json       The full request JSON (must contain a "tree" object).
    /// \param ctx        Supplies thread‑pool and store.
    /// \param dispatcher Where to register Listener nodes.
    static std::shared_ptr<INode> build(const rapidjson::Document& json,
                                        ExecutionContext* ctx,
                                        MarketDispatcher* dispatcher);
    void addListener(const std::string& symbol,
                     const std::string& field,
                     std::shared_ptr<INode> node);

  private:
    /// Recursively builds nodes, chaining \p downstream at the tail.
    static std::shared_ptr<INode> buildInternal(const rapidjson::Value&   nodeJson,
                                                 ExecutionContext*          ctx,
                                                 MarketDispatcher*         dispatcher,
                                                 std::shared_ptr<INode>    downstream);
  };
}
