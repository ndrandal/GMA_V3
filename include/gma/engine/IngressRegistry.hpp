#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace boost::asio { class io_context; }

namespace gma {
class Dispatcher;        // legacy routing hub
namespace util { struct Config; }
}

namespace gma::engine {

// Long-lived data source (TCP server, WS client, file replay, …) owned by the engine.
class IIngressSource {
public:
  virtual ~IIngressSource() = default;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

// Factory wired up at connector-registration time; invoked when engine boots an ingress.
// Signature will be refined as Dispatcher is generalized (Step 6) and Config
// gains a namespaced view (Step 7). For Step 2 scaffolding we use the types in-tree today.
using IngressFactory = std::function<std::unique_ptr<IIngressSource>(
    boost::asio::io_context& io,
    Dispatcher*        dispatcher,
    const util::Config&      config)>;

class IngressRegistry {
public:
  // Tag instance — see EventTypeRegistry::singleton() for rationale.
  static IngressRegistry& singleton() {
    static IngressRegistry s;
    return s;
  }

  static bool registerIngress(std::string kind, IngressFactory fn) {
    std::lock_guard lk(mx());
    auto [it, ok] = map().emplace(std::move(kind), std::move(fn));
    return ok;
  }

  static const IngressFactory* find(std::string_view kind) {
    std::lock_guard lk(mx());
    auto it = map().find(std::string(kind));
    return it == map().end() ? nullptr : &it->second;
  }

  static bool contains(std::string_view kind) {
    std::lock_guard lk(mx());
    return map().count(std::string(kind)) > 0;
  }

  static std::vector<std::string> kinds() {
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
  static std::unordered_map<std::string, IngressFactory>& map() {
    static std::unordered_map<std::string, IngressFactory> m;
    return m;
  }
  static std::mutex& mx() {
    static std::mutex m;
    return m;
  }
};

} // namespace gma::engine
