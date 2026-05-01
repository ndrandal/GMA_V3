#pragma once
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gma::engine {

struct EventSchema {
  std::string              name;
  std::vector<std::string> knownFields;
  bool                     dispatchable { true };
};

class EventTypeRegistry {
public:
  // Tag instance — registry methods are static, so the singleton is purely a
  // handle for EngineRegistries to expose. All real storage lives in the
  // private static map below.
  static EventTypeRegistry& singleton() {
    static EventTypeRegistry s;
    return s;
  }

  static bool registerEvent(EventSchema schema) {
    std::lock_guard lk(mx());
    auto key = schema.name;
    auto [it, ok] = map().emplace(std::move(key), std::move(schema));
    return ok;
  }

  static const EventSchema* find(std::string_view name) {
    std::lock_guard lk(mx());
    auto it = map().find(std::string(name));
    return it == map().end() ? nullptr : &it->second;
  }

  static bool contains(std::string_view name) {
    std::lock_guard lk(mx());
    return map().count(std::string(name)) > 0;
  }

  static std::vector<std::string> names() {
    std::lock_guard lk(mx());
    std::vector<std::string> out;
    out.reserve(map().size());
    for (auto& kv : map()) out.push_back(kv.first);
    return out;
  }

  static void clear() {
    std::lock_guard lk(mx());
    map().clear();
  }

private:
  static std::unordered_map<std::string, EventSchema>& map() {
    static std::unordered_map<std::string, EventSchema> m;
    return m;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma::engine
