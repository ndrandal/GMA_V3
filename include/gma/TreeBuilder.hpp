#pragma once
#include <memory>
#include <string>

namespace rapidjson { class Value; }

namespace gma {
class INode;
class AtomicStore;
class ThreadPool;
class MarketDispatcher;
}

namespace gma::tree {

struct Deps {
  gma::AtomicStore*      store{nullptr};
  gma::ThreadPool*       pool{nullptr};
  gma::MarketDispatcher* dispatcher{nullptr};
};

struct BuiltChain {
  std::shared_ptr<gma::INode> head;     // first node (Listener)
  std::shared_ptr<gma::INode> terminal; // final node (Responder)
};

// Listener(symbol, field) -> terminal
BuiltChain buildSimple(const std::string& symbol,
                       const std::string& field,
                       const Deps& deps,
                       std::shared_ptr<gma::INode> terminal);

// Build from requestJson containing "symbol" and "field"
BuiltChain buildForRequest(const rapidjson::Value& requestJson,
                           const Deps& deps,
                           std::shared_ptr<gma::INode> terminal);

} // namespace gma::tree
