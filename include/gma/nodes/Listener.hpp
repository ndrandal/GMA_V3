#pragma once

#include <queue>
#include <mutex>
#include <atomic>
#include <memory>
#include "gma/rt/SPSCQueue.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/SymbolValue.hpp"          // for SymbolValue
#include "gma/MarketDispatcher.hpp"

namespace gma::nodes {

class Listener 
  : public INode
  , public std::enable_shared_from_this<Listener>
{
public:
  Listener(const std::string& symbol,
           const std::string& field,
           std::shared_ptr<INode> downstream,
           ThreadPool* pool,
           gma::MarketDispatcher* dispatcher);
  ~Listener() override;

  void onValue(const SymbolValue& sv) override;
  void schedule();
  void shutdown() noexcept override;

  std::string _symbol;
  std::string _field;
  std::weak_ptr<INode> _downstream;
  ThreadPool* _pool;
  gma::MarketDispatcher* _dispatcher;

  std::queue<SymbolValue> _pending;    // now queue full SymbolValue
  std::mutex _mutex;
  std::atomic<bool> _running{false};

private:
  gma::rt::SPSCQueue<SymbolValue> q_{1024}; // tune capacity
  std::atomic<bool> scheduled_{false};

  // metrics (atomic to be cheap to read)
  std::atomic<uint64_t> enq_{0}, deq_{0}, dropped_{0};
};

} // namespace gma::nodes
