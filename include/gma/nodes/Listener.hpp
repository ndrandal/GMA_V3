#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "gma/nodes/INode.hpp"

namespace gma {
class MarketDispatcher;
class ThreadPool;

namespace nodes {

/**
 * Listener subscribes to a (symbol, field) and forwards values to a downstream node.
 * This header is minimal and avoids depending on a specific SPSCQueue interface.
 */
class Listener final : public INode,
                       public std::enable_shared_from_this<Listener> {
public:
  Listener(std::string symbol,
           std::string field,
           std::shared_ptr<INode> downstream,
           gma::ThreadPool* pool,
           gma::MarketDispatcher* dispatcher);

  // INode
  void onValue(const SymbolValue& sv) override;
  void shutdown() noexcept override;

  const std::string& symbol() const noexcept { return symbol_; }
  const std::string& field() const noexcept { return field_; }

private:
  std::string symbol_;
  std::string field_;

  std::weak_ptr<INode> downstream_;
  gma::ThreadPool* pool_;
  gma::MarketDispatcher* dispatcher_;

  std::atomic<bool> stopping_{false};
};

} // namespace nodes
} // namespace gma
