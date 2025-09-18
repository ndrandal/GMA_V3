#pragma once
#include <memory>

// Forward decls to avoid heavy includes here
namespace rapidjson { class Value; }

namespace gma {
class INode;
class AtomicStore;
class ThreadPool;
class MarketDispatcher;
}

namespace gma::tree {

// Dependencies that the builder needs to wire nodes
struct Deps {
  gma::AtomicStore*     store       = nullptr;
  gma::ThreadPool*      pool        = nullptr;
  gma::MarketDispatcher* dispatcher = nullptr;
};

// The result of composing a request pipeline that will be fed by MarketDispatcher
struct BuiltChain {
  std::shared_ptr<gma::INode> head;   // register this with (symbol, field)
};

// Build a single node graph described by 'spec', ending at 'terminal'
std::shared_ptr<gma::INode>
buildNode(const rapidjson::Value& spec,
          const std::string& defaultSymbol,
          const Deps& deps,
          std::shared_ptr<gma::INode> terminal);

// Convenience: AtomicAccessor (optionally polled by Interval) -> terminal
std::shared_ptr<gma::INode>
buildSimple(const std::string& symbol,
            const std::string& field,
            int pollMs,
            const Deps& deps,
            std::shared_ptr<gma::INode> terminal);

// NEW: Build a dispatcher-facing Listener head for requestJson ("symbol","field"),
// optionally prepend a pipeline, and finish at 'terminal' (e.g., Responder).
BuiltChain
buildForRequest(const rapidjson::Value& requestJson,
                const Deps& deps,
                std::shared_ptr<gma::INode> terminal);

} // namespace gma::tree
