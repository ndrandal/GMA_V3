#pragma once
#include <string_view>
#include "gma/engine/EngineRegistries.hpp"

namespace gma::engine {

// Contract every connector implements. The composition root constructs one
// instance per linked connector and calls registerWith() once at boot. The
// connector publishes its event types, computers, node types, config readers,
// and ingress sources through the various engine registries (most of which
// are static singletons, so not passed via EngineRegistries).
class IConnector {
public:
  virtual ~IConnector() = default;

  virtual std::string_view name() const = 0;
  virtual void registerWith(EngineRegistries&) = 0;
};

} // namespace gma::engine
