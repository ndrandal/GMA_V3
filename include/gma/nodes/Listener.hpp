#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "gma/nodes/INode.hpp"
#include "gma/rt/SPSCQueue.hpp"

namespace gma {
class MarketDispatcher;
namespace rt { class ThreadPool; }

namespace nodes {

class Listener final : public INode,
                       public std::enable_shared_from_this<Listener> {
public:
  Listener(std::string symbol,
           std::string field,
           std::shared_ptr<INode> downstream,
           gma::rt::ThreadPool* pool,
           gma::MarketDispatcher* dispatcher,
           std::size_t queueCap = 1024);

  // Called by the dispatcher thread (or any producer thread)
  void onValue(const SymbolValue& sv) override;

  // Unsubscribe + drain
  void shutdown() noexcept override;

private:
  void schedulePump();
  void pumpOnce();

  const std::string symbol_;
  const std::string field_;
  std::weak_ptr<INode> downstream_;
  gma::rt::ThreadPool* pool_;
  gma::MarketDispatcher* dispatcher_;

  gma::rt::SPSCQueue<SymbolValue> q_;
  std::atomic<bool> scheduled_{false};
  std::atomic<bool> stopping_{false};

  // lightweight counters (optional)
  std::atomic<uint64_t> enq_{0}, deq_{0}, dropped_{0};
};

} // namespace nodes
} // namespace gma
