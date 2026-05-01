#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "gma/engine/IEventComputer.hpp"

namespace gma::engine {

// Registry of **factories** that produce per-dispatcher IEventComputer instances.
// Dispatchers call createAll() during construction to get their own fresh set of
// computers — this isolates state (e.g. per-symbol TA histories) between
// dispatcher instances, which matters in tests that spin up multiple dispatchers.
class EventComputerRegistry {
public:
  using Factory = std::function<std::unique_ptr<IEventComputer>()>;

  // Tag instance — see EventTypeRegistry::singleton() for rationale.
  static EventComputerRegistry& singleton() {
    static EventComputerRegistry s;
    return s;
  }

  // Register a factory for an event type. Multiple factories per type are
  // retained in registration order. Always succeeds.
  static void registerFactory(std::string eventType, Factory factory) {
    if (!factory) return;
    std::lock_guard lk(mx());
    map()[std::move(eventType)].push_back(std::move(factory));
  }

  // Instantiate every registered computer for the given event type, in
  // registration order. Each call yields fresh instances.
  static std::vector<std::unique_ptr<IEventComputer>>
  createAll(std::string_view eventType) {
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
      if (auto c = f()) out.push_back(std::move(c));
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
