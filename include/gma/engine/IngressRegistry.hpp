#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gma::engine {

struct EngineRegistries;

// Long-lived data source (TCP server, WS client, file replay, …) owned by the engine.
class IIngressSource {
public:
  virtual ~IIngressSource() = default;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

// Per-instance INI-style parameters for one ingress entry, e.g. {"port":"9001"}
// for `ingress.0.kind = market.feedserver, ingress.0.port = 9001`. Engine
// parses the entry's sub-keys and passes the map to the factory.
using IngressParams = std::unordered_map<std::string, std::string>;

// Factory wired up at connector-registration time; invoked once per
// cfg.ingress[] entry of this kind. Receives the engine handles plus the
// entry's parsed params. Connectors typically close over their own state
// (OrderBookManager, SnapshotSource, …) at registration time and pull
// engine handles (io, dispatcher) out of regs at instantiation time.
using IngressFactory = std::function<std::unique_ptr<IIngressSource>(
    EngineRegistries& regs,
    const IngressParams& params)>;

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
