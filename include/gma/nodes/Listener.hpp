#pragma once

#include <queue>
#include <mutex>
#include <atomic>
#include <memory>

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
};

} // namespace gma::nodes
