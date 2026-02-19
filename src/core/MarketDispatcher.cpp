#include "gma/MarketDispatcher.hpp"
#include "gma/AtomicFunctions.hpp"
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
                                   AtomicStore* store,
                                   const util::Config& cfg)
  : _threadPool(threadPool)
  , _store(store)
  , _cfg(cfg) {}

void MarketDispatcher::registerListener(const std::string& symbol,
                                        const std::string& field,
                                        std::shared_ptr<INode> listener)
{
  std::unique_lock<std::shared_mutex> lock(_listenerMutex);
  _listeners[symbol][field].emplace_back(std::move(listener));
}

void MarketDispatcher::unregisterListener(const std::string& symbol,
                                          const std::string& field,
                                          std::shared_ptr<INode> listener)
{
  std::unique_lock<std::shared_mutex> lock(_listenerMutex);
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
  // Early-out: reject empty symbols and null payloads before touching any state.
  if (tick.symbol.empty() || !tick.payload) return;

  // Collect (field, listener) pairs for this symbol — snapshot under listener lock
  std::vector<std::pair<std::string, std::shared_ptr<INode>>> toNotify;

  {
    std::shared_lock<std::shared_mutex> lock(_listenerMutex);
    auto lit = _listeners.find(tick.symbol);
    if (lit != _listeners.end()) {
      for (auto& kv : lit->second) {
        const std::string& field = kv.first;
        for (auto& sp : kv.second) {
          if (tick.payload->HasMember(field.c_str())) {
            toNotify.emplace_back(field, sp);
          }
        }
      }
    }
  }

  // Update symbol-level history and run full TA suite
  updateSymbolHistory(tick.symbol, tick);

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

    // Update per-field history and build contiguous vector under history lock
    std::vector<double> histVec;
    {
      std::unique_lock<std::shared_mutex> lock(_histMutex);
      // Cap distinct symbol count to bound memory growth.
      if (_histories.find(tick.symbol) == _histories.end() &&
          _histories.size() >= MAX_SYMBOLS) {
        continue;
      }
      auto& hist = _histories[tick.symbol][field];
      hist.push_back(raw);
      if (hist.size() > MAX_HISTORY) hist.pop_front();
      histVec.assign(hist.begin(), hist.end());
    }

    // Compute per-field atomics on the contiguous snapshot (no lock needed)
    computeAndStoreAtomics(tick.symbol, field, histVec);

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

void MarketDispatcher::updateSymbolHistory(const std::string& symbol,
                                           const SymbolTick& tick) {
  if (!_store || !tick.payload) return;

  // Extract price and volume from the tick payload
  double price  = 0.0;
  double volume = 0.0;
  bool hasPrice = false;

  const auto& doc = *tick.payload;

  // Try common price field names
  for (const char* pf : {"lastPrice", "price", "last", "px"}) {
    if (doc.HasMember(pf) && doc[pf].IsNumber()) {
      price = doc[pf].GetDouble();
      hasPrice = true;
      break;
    }
  }

  if (!hasPrice) return; // No price data — skip TA computation

  // Try common volume field names
  for (const char* vf : {"volume", "vol", "qty", "size"}) {
    if (doc.HasMember(vf) && doc[vf].IsNumber()) {
      volume = doc[vf].GetDouble();
      break;
    }
  }

  // Update symbol history under lock, then compute TA outside lock.
  // Snapshot into a vector for contiguous memory and better cache locality.
  std::vector<TickEntry> histVec;
  {
    std::unique_lock<std::shared_mutex> lock(_histMutex);
    // Cap distinct symbol count to bound memory growth.
    if (_symbolHistories.find(symbol) == _symbolHistories.end() &&
        _symbolHistories.size() >= MAX_SYMBOLS) {
      return;
    }
    auto& hist = _symbolHistories[symbol];
    hist.push_back(TickEntry{price, volume});
    if (hist.size() > MAX_HISTORY) hist.pop_front();
    histVec.assign(hist.begin(), hist.end());
  }

  // Run the full TA suite — writes results to AtomicStore via setBatch
  computeAllAtomicValues(symbol, histVec, *_store, _cfg);
}

void MarketDispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::string& /*field*/,
                                              const std::vector<double>& history)
{
  auto& fmap = FunctionMap::instance();

  // Snapshot all listeners for this symbol once, avoiding repeated lock
  // acquisitions inside the forEach loop (one lock instead of N).
  std::map<std::string, std::vector<std::shared_ptr<INode>>> symListeners;
  {
    std::shared_lock<std::shared_mutex> lock(_listenerMutex);
    auto sit = _listeners.find(symbol);
    if (sit != _listeners.end()) {
      symListeners = sit->second;
    }
  }

  // Iterate under FunctionMap's shared_lock via forEach — no copy of std::function objects
  fmap.forEach([&](const std::string& fnName, const Func& fn) {
    if (!fn) return;

    double result = 0.0;
    try {
      result = fn(history);  // history is already a contiguous vector — no conversion
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "MarketDispatcher: atomic function error",
                              { {"symbol", symbol}, {"fn", fnName},
                                {"err", ex.what()} });
      return;
    }

    if (_store) {
      _store->set(symbol, fnName, result);
    }

    // Notify subscribers for (symbol, fnName) from the snapshot
    auto fit = symListeners.find(fnName);
    if (fit == symListeners.end()) return;

    for (auto& listener : fit->second) {
      if (_threadPool) {
        _threadPool->post([listener, symbol, result]() {
          if (listener) listener->onValue(SymbolValue{ symbol, result });
        });
      } else {
        if (listener) listener->onValue(SymbolValue{ symbol, result });
      }
    }
  });
}
