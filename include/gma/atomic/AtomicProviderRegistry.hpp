#pragma once
#include <string>
#include <unordered_map>
#include <functional>
#include <optional>
#include <mutex>

namespace gma {

// Minimal, thread-safe registry that lets AtomicAccessor resolve keys by namespace (prefix before first '.')
class AtomicProviderRegistry {
public:
  using ProviderFn = std::function<double(const std::string& symbol, const std::string& fullKey)>;

  // Register (or replace) a provider function for a namespace, e.g., "ema", "vwap"
  static void registerNamespace(const std::string& ns, ProviderFn fn) {
    std::lock_guard<std::mutex> lk(mx());
    map()[ns] = std::move(fn);
  }

  // Remove a namespace; returns true if something was erased
  static bool unregisterNamespace(const std::string& ns) {
    std::lock_guard<std::mutex> lk(mx());
    return map().erase(ns) > 0;
  }

  // Clear all providers (primarily for tests or full reinit)
  static void clear() {
    std::lock_guard<std::mutex> lk(mx());
    map().clear();
  }

  // Try to resolve and evaluate a key of the form "<ns>.<rest>"
  // Returns std::nullopt if no provider is found.
  static std::optional<double> tryResolve(const std::string& symbol, const std::string& key) {
    auto dot = key.find('.');
    if (dot == std::string::npos) return std::nullopt;
    const std::string ns = key.substr(0, dot);

    // Copy the provider function under lock, then call outside lock
    // to avoid blocking all concurrent resolves while one provider executes.
    ProviderFn fn;
    {
      std::lock_guard<std::mutex> lk(mx());
      auto it = map().find(ns);
      if (it == map().end()) return std::nullopt;
      fn = it->second;
    }
    try {
      return fn(symbol, key);
    } catch (...) {
      return std::nullopt;
    }
  }

private:
  static std::unordered_map<std::string, ProviderFn>& map() {
    static std::unordered_map<std::string, ProviderFn> instance;
    return instance;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma
