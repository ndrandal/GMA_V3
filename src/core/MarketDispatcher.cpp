#include "gma/MarketDispatcher.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/AtomicFunctions.hpp"
#include "gma/SymbolTick.hpp"
#include <algorithm>
#include <shared_mutex>
#include <deque>

using namespace gma;


MarketDispatcher::MarketDispatcher(ThreadPool* threadPool,
                                   AtomicStore* store)
  : _threadPool(threadPool)
  , _store(store)
{}

MarketDispatcher::~MarketDispatcher() = default;

void MarketDispatcher::registerListener(const std::string& symbol,
                                        const std::string& field,
                                        std::shared_ptr<INode> listener)
{
  std::unique_lock<std::shared_mutex> lock(_mutex);
  _listeners[symbol][field].emplace_back(std::move(listener));
}

void MarketDispatcher::unregisterListener(const std::string& symbol,
                                          const std::string& field,
                                          std::shared_ptr<INode> listener)
{
  std::unique_lock<std::shared_mutex> lock(_mutex);
  auto symIt = _listeners.find(symbol);
  if (symIt == _listeners.end()) return;
  auto& fieldMap = symIt->second;
  auto fldIt = fieldMap.find(field);
  if (fldIt == fieldMap.end()) return;
  auto& vec = fldIt->second;
  vec.erase(std::remove(vec.begin(), vec.end(), listener), vec.end());
  if (vec.empty()) fieldMap.erase(fldIt);
  if (fieldMap.empty()) _listeners.erase(symIt);
}

void MarketDispatcher::onTick(const SymbolTick& tick) {
  // Collect (field, listener) pairs for this symbol
  std::vector<std::pair<std::string, std::shared_ptr<INode>>> toNotify;
  {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto lit = _listeners.find(tick.symbol);
    if (lit != _listeners.end()) {
      for (auto& [field, vec] : lit->second) {
        for (auto& node : vec) {
          toNotify.emplace_back(field, node);
        }
      }
    }
  }

  // Offload extraction, history update, computation, and notification
  _threadPool->post([this, tick, toNotify = std::move(toNotify)]() {
    for (auto& [field, node] : toNotify) {
      // 1) Extract the desired numeric value from the JSON payload
      double raw = 0.0;
      try {
        double raw = (*tick.payload)[field.c_str()].GetDouble();
      } catch (...) {
        continue;
      }

      // 2) Update per-(symbol,field) history
      auto& hist = _histories[tick.symbol][field];
      hist.push_back(raw);
      if (hist.size() > MAX_HISTORY) hist.pop_front();

      // 3) Recompute all atomic functions for this (symbol,field)
      computeAndStoreAtomics(tick.symbol, field, hist);

      // 4) Notify the raw-value listener
      SymbolValue out{ tick.symbol, raw };
      node->onValue(out);
    }
  });
}

void MarketDispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::string& field,
                                              const std::deque<double>& history)
{
  // Iterate all registered atomic functions
  for (auto const& [fnName, fnPtr] : FunctionMap::instance().getAll()) {
    // Compute indicator value on this field's history
    double result = fnPtr(std::vector<double>(history.begin(), history.end()));

    // Store into AtomicStore under (symbol, fnName)
    _store->set(symbol, fnName, result);

    // Gather listeners who subscribed to this atomic (by fnName)
    std::vector<std::shared_ptr<INode>> subs;
    {
      std::shared_lock<std::shared_mutex> lock(_mutex);
      auto sit = _listeners.find(symbol);
      if (sit != _listeners.end()) {
        auto fit = sit->second.find(fnName);
        if (fit != sit->second.end()) {
          subs = fit->second;
        }
      }
    }

    // Dispatch the atomic result to each subscriber
    for (auto& listener : subs) {
      _threadPool->post([listener, symbol, result]() {
        listener->onValue(SymbolValue{ symbol, result });
      });
    }
  }
}
