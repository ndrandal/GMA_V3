#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <shared_mutex>
#include <deque>

namespace gma {
  struct SymbolValue;
  class ThreadPool;
  class AtomicStore;
  class INode;                   // forward‑declared so we can use shared_ptr

  namespace nodes { class Listener; }

  /// Dispatches raw ticks into symbol‑/field‑scoped listeners.
  class MarketDispatcher {
  public:
    MarketDispatcher(ThreadPool* threadPool, AtomicStore* store);
    ~MarketDispatcher();

    void registerListener(const std::string& symbol,
                          const std::string& field,
                          std::shared_ptr<INode> listener);
    void unregisterListener(const std::string& symbol,
                            const std::string& field,
                            std::shared_ptr<INode> listener);

    void onTick(const SymbolValue& tick);
    void computeAndStoreAtomics(const std::string& symbol, const std::deque<double>& history);
    
    std::deque<double> getHistoryCopy(const std::string& symbol) const;
    void addListener(const std::string& symbol,
                     const std::string& field,
                     std::shared_ptr<INode> node);

  private:
    // Copy of recent raw values per symbol
    std::unordered_map<std::string, std::deque<double>> _histories;

    // Map: symbol → field → vector of listener nodes
    std::unordered_map<std::string,
      std::unordered_map<std::string,
        std::vector<std::shared_ptr<INode>>>>
      _listeners;

    mutable std::shared_mutex _mutex;
    ThreadPool*   _threadPool;
    AtomicStore*  _store;

  };
}
