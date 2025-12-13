#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/SymbolTick.hpp"

#include <algorithm>
#include <shared_mutex>
#include <deque>

using namespace gma;

MarketDispatcher::MarketDispatcher(rt::ThreadPool* threadPool,
                                   AtomicStore* store)
  : _threadPool(threadPool)
  , _store(store) {}

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
  // Collect (field, listener) pairs for this symbol — we’ll notify outside the lock
  std::vector<std::pair<std::string, std::shared_ptr<INode>>> toNotify;

  {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto lit = _listeners.find(tick.symbol);
    if (lit != _listeners.end()) {
      // If any subscribers registered to specific raw fields, they must exist as keys
      for (auto& kv : lit->second) {
        const std::string& field = kv.first;
        for (auto& sp : kv.second) {
          // Capture only raw fields that appear in the payload; function results will be
          // dispatched later by computeAndStoreAtomics using their function names.
          if (tick.payload && tick.payload->HasMember(field.c_str())) {
            toNotify.emplace_back(field, sp);
          }
        }
      }
    }
  }

  // Process raw values for the (symbol, field) and update history
  if (tick.payload) {
    for (auto& [field, node] : toNotify) {
      double raw = 0.0;
      try {
        const auto& v = (*tick.payload)[field.c_str()];
        if (!v.IsNumber()) continue;
        raw = v.GetDouble();
      } catch (...) {
        continue;
      }

      // Update per-(symbol,field) history
      std::deque<double>* histPtr = nullptr;
      {
        std::unique_lock<std::shared_mutex> lock(_mutex);
        auto& hist = _histories[tick.symbol][field];
        hist.push_back(raw);
        if (hist.size() > MAX_HISTORY) hist.pop_front();
        histPtr = &hist;
      }

      // Recompute all atomic functions for this (symbol,field)
      computeAndStoreAtomics(tick.symbol, field, *histPtr);

      // Notify the raw-value subscriber
      SymbolValue out{ tick.symbol, raw };
      if (_threadPool) {
        _threadPool->post([node, out]() {
          if (node) node->onValue(out);
        });
      } else {
        if (node) node->onValue(out);
      }
    }
  }
}

void MarketDispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::string& field,
                                              const std::deque<double>& history)
{
  // Snapshot registered atomic functions
  std::vector<std::pair<std::string, Func>> funcs = FunctionMap::instance().getAll();

  for (auto& [fnName, fn] : funcs) {
    // Defensive: skip empty
    if (!fn) continue;

    // Compute atomic result
    std::vector<double> vec(history.begin(), history.end());
    double result = 0.0;
    try {
      result = fn(vec);
    } catch (...) {
      continue;
    }

    // Store under key == function name (users subscribe to fnName)
    if (_store) {
      _store->set(symbol, fnName, result);
    }

    // Collect subscribers for (symbol, fnName)
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
      if (_threadPool) {
        _threadPool->post([listener, symbol, result]() {
          if (listener) listener->onValue(SymbolValue{ symbol, result });
        });
      } else {
        if (listener) listener->onValue(SymbolValue{ symbol, result });
      }
    }
  }
}
