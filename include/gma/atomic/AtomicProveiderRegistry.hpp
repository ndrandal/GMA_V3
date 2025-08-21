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

  static void registerNamespace(const std::string& ns, ProviderFn fn){
    std::lock_guard<std::mutex> lk(mx());
    map()[ns] = std::move(fn);
  }

  static std::optional<double> tryResolve(const std::string& fullKey, const std::string& symbol){
    auto pos = fullKey.find('.');
    if(pos==std::string::npos) return std::nullopt;
    std::string ns = fullKey.substr(0, pos);
    std::lock_guard<std::mutex> lk(mx());
    auto it = map().find(ns);
    if(it==map().end()) return std::nullopt;
    return it->second(symbol, fullKey);
  }

private:
  static std::unordered_map<std::string, ProviderFn>& map(){
    static std::unordered_map<std::string, ProviderFn> m; return m;
  }
  static std::mutex& mx(){ static std::mutex m; return m; }
};

} // namespace gma
