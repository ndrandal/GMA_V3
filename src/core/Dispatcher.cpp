#include "gma/Dispatcher.hpp"
#include "gma/util/Logger.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

using namespace gma;

void Dispatcher::addComputer(std::unique_ptr<engine::IEventComputer> c) {
  if (c) _computers.push_back(std::move(c));
}

Dispatcher::Dispatcher(rt::ThreadPool* threadPool,
                                   AtomicStore* store,
                                   const util::Config& cfg)
  : _threadPool(threadPool)
  , _store(store)
  , _cfg(cfg)
  , _maxHistory(static_cast<std::size_t>(std::max(1, cfg.taHistoryMax)))
  , _maxSymbols(static_cast<std::size_t>(std::max(1, cfg.maxSymbols)))
  , _maxFieldsPerSymbol(static_cast<std::size_t>(std::max(1, cfg.maxFieldsPerSymbol)))
{}

void Dispatcher::registerListener(const std::string& symbol,
                                        const std::string& field,
                                        std::shared_ptr<INode> listener)
{
  std::unique_lock<std::shared_mutex> lock(_listenerMutex);
  _listeners[symbol][field].emplace_back(std::move(listener));
}

void Dispatcher::unregisterListener(const std::string& symbol,
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

void Dispatcher::onTick(const Event& tick) {
  if (tick.symbol.empty() || !tick.payload) return;

  engine::ComputeContext ctx{ _store, this, _threadPool };

  // Per-type cache fed from EventComputerRegistry. First event of a given
  // type instantiates the registered factories; subsequent events reuse the
  // cached instances. Late-registered factories are picked up the first time
  // an event of their type arrives.
  std::vector<engine::IEventComputer*> typedComputers;
  {
    std::lock_guard<std::mutex> lk(_computerCacheMx);
    auto it = _computersByType.find(tick.type);
    if (it == _computersByType.end()) {
      auto fresh = engine::EventComputerRegistry::createAll(tick.type, _cfg);
      it = _computersByType.emplace(tick.type, std::move(fresh)).first;
    }
    typedComputers.reserve(it->second.size());
    for (auto& c : it->second) typedComputers.push_back(c.get());
  }
  for (auto* c : typedComputers) {
    if (c) c->compute(tick, ctx);
  }

  // Computers added directly via addComputer() — kept for tests and code
  // paths that want to inject without the global registry.
  for (auto& c : _computers) {
    if (!c) continue;
    if (c->eventType() != tick.type) continue;
    c->compute(tick, ctx);
  }

  // Collect (field, listener) pairs for this symbol under the listener lock.
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

  for (auto& [field, node] : toNotify) {
    double raw = 0.0;
    try {
      const auto& v = (*tick.payload)[field.c_str()];
      if (!v.IsNumber()) continue;
      raw = v.GetDouble();
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "Dispatcher: tick field read error",
                              { {"symbol", tick.symbol}, {"field", field},
                                {"err", ex.what()} });
      continue;
    }

    std::vector<double> histVec;
    {
      std::unique_lock<std::shared_mutex> lock(_histMutex);
      if (_histories.find(tick.symbol) == _histories.end() &&
          _histories.size() >= _maxSymbols) {
        continue;
      }
      auto& symFields = _histories[tick.symbol];
      if (symFields.find(field) == symFields.end() &&
          symFields.size() >= _maxFieldsPerSymbol) {
        continue;
      }
      auto& hist = symFields[field];
      hist.push_back(raw);
      if (hist.size() > _maxHistory) hist.pop_front();
      histVec.assign(hist.begin(), hist.end());
    }

    computeAndStoreAtomics(tick.symbol, field, histVec);

    StreamValue out{ tick.symbol, raw };
    if (_threadPool) {
      _threadPool->post([node, out]() {
        if (node) node->onValue(out);
      });
    } else {
      if (node) node->onValue(out);
    }
  }
}

void Dispatcher::notifyListeners(const std::string& symbol,
                                       const std::string& field,
                                       double value) {
  std::vector<std::shared_ptr<INode>> targets;
  {
    std::shared_lock<std::shared_mutex> lock(_listenerMutex);
    auto sit = _listeners.find(symbol);
    if (sit == _listeners.end()) return;
    auto fit = sit->second.find(field);
    if (fit == sit->second.end()) return;
    targets = fit->second;
  }
  if (targets.empty()) return;

  StreamValue out{ symbol, value };
  for (auto& node : targets) {
    if (_threadPool) {
      _threadPool->post([node, out]() {
        if (node) node->onValue(out);
      });
    } else {
      if (node) node->onValue(out);
    }
  }
}

void Dispatcher::computeAndStoreAtomics(const std::string& symbol,
                                              const std::string& /*field*/,
                                              const std::vector<double>& history)
{
  auto& fmap = FunctionMap::instance();

  std::map<std::string, std::vector<std::shared_ptr<INode>>> symListeners;
  {
    std::shared_lock<std::shared_mutex> lock(_listenerMutex);
    auto sit = _listeners.find(symbol);
    if (sit != _listeners.end()) {
      symListeners = sit->second;
    }
  }

  fmap.forEach([&](const std::string& fnName, const Func& fn) {
    if (!fn) return;

    double result = 0.0;
    try {
      result = fn(history);
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
                              "Dispatcher: atomic function error",
                              { {"symbol", symbol}, {"fn", fnName},
                                {"err", ex.what()} });
      return;
    }

    if (_store) {
      _store->set(symbol, fnName, result);
    }

    auto fit = symListeners.find(fnName);
    if (fit == symListeners.end()) return;

    for (auto& listener : fit->second) {
      if (_threadPool) {
        _threadPool->post([listener, symbol, result]() {
          if (listener) listener->onValue(StreamValue{ symbol, result });
        });
      } else {
        if (listener) listener->onValue(StreamValue{ symbol, result });
      }
    }
  });
}
