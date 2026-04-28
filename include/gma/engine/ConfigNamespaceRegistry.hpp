#pragma once
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gma::engine {

// Connector-supplied reader for keys under a registered prefix.
// The reader receives the key tail (portion after the first '.') and the raw value.
// Return true if the key was consumed; false lets the engine decide whether to warn.
using ConfigReaderFn = std::function<bool(std::string_view keyTail,
                                          std::string_view value)>;

class ConfigNamespaceRegistry {
public:
  static bool registerNamespace(std::string prefix, ConfigReaderFn reader) {
    std::lock_guard lk(mx());
    auto [it, ok] = map().emplace(std::move(prefix), std::move(reader));
    return ok;
  }

  // Route a single key=value observation to its matching reader.
  // Returns true when a reader was found and it accepted the key.
  static bool dispatch(std::string_view fullKey, std::string_view value) {
    const auto dot = fullKey.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string prefix(fullKey.substr(0, dot));
    const std::string_view tail = fullKey.substr(dot + 1);

    ConfigReaderFn fn;
    {
      std::lock_guard lk(mx());
      auto it = map().find(prefix);
      if (it == map().end()) return false;
      fn = it->second;
    }
    try {
      return fn(tail, value);
    } catch (...) {
      return false;
    }
  }

  static bool contains(std::string_view prefix) {
    std::lock_guard lk(mx());
    return map().count(std::string(prefix)) > 0;
  }

  static std::vector<std::string> prefixes() {
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
  static std::unordered_map<std::string, ConfigReaderFn>& map() {
    static std::unordered_map<std::string, ConfigReaderFn> m;
    return m;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma::engine
