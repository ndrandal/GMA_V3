#pragma once
#include <memory>
#include <string>

// Forward decls
class INode;
namespace rapidjson { class Value; }

namespace gma {
  class AtomicStore;
  class MarketDispatcher;
  namespace rt { class ThreadPool; }
}

namespace gma::tree {

struct Deps {
  gma::AtomicStore*         store   {nullptr};
  gma::MarketDispatcher*    dispatcher {nullptr};
  gma::rt::ThreadPool*      pool    {nullptr};
};

// Build from a JSON node spec and return the **root** that you should attach to your graph.
// `defaultSymbol` is used if a spec omits "symbol".
// `terminal` is the downstream tail (e.g., a WS responder); TreeBuilder will wire nodes **toward** it.
std::shared_ptr<INode> buildNode(const rapidjson::Value& spec,
                                 const std::string& defaultSymbol,
                                 const Deps& deps,
                                 std::shared_ptr<INode> terminal);

// Convenience: short-form builder used by WS for {symbol, field, pollMs}
std::shared_ptr<INode> buildSimple(const std::string& symbol,
                                   const std::string& field,
                                   int pollMs,  // <=0 means push mode (no Interval)
                                   const Deps& deps,
                                   std::shared_ptr<INode> terminal);

} // namespace gma::tree
