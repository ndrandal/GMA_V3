#include "gma/MarketDispatcher.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/FunctionMap.hpp"    // for FunctionMap::getAll()
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include "gma/AtomicFunctions.hpp"
#include <algorithm>
#include <variant>

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
  std::unique_lock lock(_mutex);
  _listeners[symbol][field].emplace_back(std::move(listener));
}

void MarketDispatcher::unregisterListener(const std::string& symbol,
                                          const std::string& field,
                                          std::shared_ptr<INode> listener)
{
  std::unique_lock lock(_mutex);
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

void MarketDispatcher::onTick(const SymbolValue& tick)
{
  std::vector<std::shared_ptr<INode>> toNotify;
  {
    std::shared_lock lock(_mutex);

    // Extract raw double
    double raw = 0.0;
    if (auto pd = std::get_if<double>(&tick.value)) {
      raw = *pd;
    }

    auto& history = _histories[tick.symbol];
    history.push_back(raw);
    if (history.size() > 1000) history.pop_front();

    auto lit = _listeners.find(tick.symbol);
    if (lit != _listeners.end()) {
      for (auto& [field, vec] : lit->second) {
        toNotify.insert(toNotify.end(), vec.begin(), vec.end());
      }
    }
  }

  _threadPool->post([=]() {
    auto histCopy = getHistoryCopy(tick.symbol);
    computeAndStoreAtomics(tick.symbol, histCopy);
    for (auto& listener : toNotify) {
      listener->onValue(tick);
    }
  });
}

std::deque<double>
MarketDispatcher::getHistoryCopy(const std::string& symbol) const
{
  std::shared_lock lock(_mutex);
  auto it = _histories.find(symbol);
  return (it == _histories.end() ? std::deque<double>() : it->second);
}

void MarketDispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::deque<double>& history)
{
  // Use the new getAll() API
  for (auto const& [fieldName, func] : FunctionMap::instance().getAll()) {
    double val = func(std::vector<double>(history.begin(), history.end()));
    _store->set(symbol, fieldName, val);
  }
}
