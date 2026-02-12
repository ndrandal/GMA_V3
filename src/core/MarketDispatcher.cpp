#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/util/Logger.hpp"

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
  // Collect (field, listener) pairs for this symbol — snapshot under lock
  std::vector<std::pair<std::string, std::shared_ptr<INode>>> toNotify;

  {
    std::shared_lock<std::shared_mutex> lock(_mutex);
    auto lit = _listeners.find(tick.symbol);
    if (lit != _listeners.end()) {
      for (auto& kv : lit->second) {
        const std::string& field = kv.first;
        for (auto& sp : kv.second) {
          if (tick.payload && tick.payload->HasMember(field.c_str())) {
            toNotify.emplace_back(field, sp);
          }
        }
      }
    }
  }

  if (!tick.payload) return;

  for (auto& [field, node] : toNotify) {
    double raw = 0.0;
    try {
      const auto& v = (*tick.payload)[field.c_str()];
      if (!v.IsNumber()) continue;
      raw = v.GetDouble();
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "MarketDispatcher: tick field read error",
                              { {"symbol", tick.symbol}, {"field", field},
                                {"err", ex.what()} });
      continue;
    }

    // Update history and take a snapshot under lock
    std::deque<double> histCopy;
    {
      std::unique_lock<std::shared_mutex> lock(_mutex);
      auto& hist = _histories[tick.symbol][field];
      hist.push_back(raw);
      if (hist.size() > MAX_HISTORY) hist.pop_front();
      histCopy = hist; // safe copy while locked
    }

    // Compute atomics on the snapshot (no lock needed)
    computeAndStoreAtomics(tick.symbol, field, histCopy);

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

void MarketDispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::string& field,
                                              const std::deque<double>& history)
{
  auto& fmap = FunctionMap::instance();

  // Iterate under the FunctionMap's lock via forEach — no copy needed
  for (const auto& [fnName, fn] : fmap.getAll()) {
    if (!fn) continue;

    std::vector<double> vec(history.begin(), history.end());
    double result = 0.0;
    try {
      result = fn(vec);
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "MarketDispatcher: atomic function error",
                              { {"symbol", symbol}, {"fn", fnName},
                                {"err", ex.what()} });
      continue;
    }

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
