#pragma once
#include <string_view>
#include "gma/engine/EngineRegistries.hpp"

namespace gma::engine {

// Contract every connector implements. The composition root constructs one
// instance per linked connector and drives it through a strict lifecycle:
//
//   1. registerWith(EngineRegistries&) — exactly once. Publish event types,
//      computers, node types, config readers, ingress sources, and atomic
//      providers via the engine registries; allocate (but do not run) any
//      sockets, timers, or background threads.
//   2. start() — exactly once, after registerWith. Bring sources online: run
//      servers, connect clients, arm timers. May throw on failure.
//   3. stop() noexcept — exactly once, after start (idempotent on extra
//      calls). Tear down everything started in start(), in reverse order.
//      Safe to call from a ShutdownCoordinator step.
//
// Static engine registries (NodeTypeRegistry, FunctionMap, …) are accessed
// through their own singletons and not re-exposed here.
class IConnector {
public:
  virtual ~IConnector() = default;

  virtual std::string_view name() const = 0;
  virtual void registerWith(EngineRegistries&) = 0;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

} // namespace gma::engine
