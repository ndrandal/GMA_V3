#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include "gma/engine/IEventComputer.hpp"
#include "gma/util/Config.hpp"

namespace gma::engine {

// Registry of **factories** that produce per-dispatcher IEventComputer instances.
// Dispatchers call createAll() on first event of each type to get their own
// fresh set of computers — this isolates state (e.g. per-symbol TA histories)
// between dispatcher instances, which matters in tests that spin up multiple
// dispatchers.
//
// Factories receive the dispatcher's util::Config so connector-supplied
// computers (e.g. MarketTickComputer) can honor per-dispatcher tuning.
// Connectors that do not need the config can register a no-arg factory via
// the convenience overload below.
class EventComputerRegistry {
public:
  using Factory = std::function<std::unique_ptr<IEventComputer>(const util::Config&)>;

  // Tag instance — see EventTypeRegistry::singleton() for rationale.
  static EventComputerRegistry& singleton() {
    static EventComputerRegistry s;
    return s;
  }

  // Register a config-aware factory for an event type. Multiple factories per
  // type are retained in registration order. Always succeeds.
  static void registerFactory(std::string eventType, Factory factory) {
    if (!factory) return;
    std::lock_guard lk(mx());
    map()[std::move(eventType)].push_back(std::move(factory));
  }

  // Convenience overload: register a no-arg factory. The dispatcher's cfg is
  // ignored. Useful for tests and computers whose construction is configless.
  static void registerFactory(std::string eventType,
                              std::function<std::unique_ptr<IEventComputer>()> nullary) {
    if (!nullary) return;
    registerFactory(std::move(eventType),
                    [f = std::move(nullary)](const util::Config&) { return f(); });
  }

  // Instantiate every registered computer for the given event type, in
  // registration order. Each call yields fresh instances. Factories receive
  // the supplied cfg (default-constructed if omitted).
  static std::vector<std::unique_ptr<IEventComputer>>
  createAll(std::string_view eventType, const util::Config& cfg = util::Config{}) {
    std::vector<Factory> factoriesCopy;
    {
      std::lock_guard lk(mx());
      auto it = map().find(std::string(eventType));
      if (it == map().end()) return {};
      factoriesCopy = it->second;
    }
    std::vector<std::unique_ptr<IEventComputer>> out;
    out.reserve(factoriesCopy.size());
    for (auto& f : factoriesCopy) {
      if (auto c = f(cfg)) out.push_back(std::move(c));
    }
    return out;
  }

  static std::size_t factoryCount(std::string_view eventType) {
    std::lock_guard lk(mx());
    auto it = map().find(std::string(eventType));
    return it == map().end() ? 0 : it->second.size();
  }

  static void clear() {
    std::lock_guard lk(mx());
    map().clear();
  }

private:
  static std::unordered_map<std::string, std::vector<Factory>>& map() {
    static std::unordered_map<std::string, std::vector<Factory>> m;
    return m;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma::engine
