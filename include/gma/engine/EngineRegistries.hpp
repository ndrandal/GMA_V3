#pragma once

namespace boost::asio { class io_context; }

namespace gma {
class AtomicProviderRegistry;
class AtomicStore;
class Dispatcher;
class FunctionMap;
namespace rt   { class ThreadPool; class ShutdownCoordinator; }
namespace util { class Config; class Logger; }
}

namespace gma::engine {

class ConfigNamespaceRegistry;
class EventComputerRegistry;
class EventTypeRegistry;
class IngressRegistry;
class NodeTypeRegistry;

// Handle passed to every IConnector::registerWith() call. Bundles every
// long-lived engine piece a connector may need during boot.
//
// The first six fields (cfg / pool / store / dispatcher / shutdown / io)
// are real per-instance objects owned by main() (or a test fixture).
//
// The next eight fields point at the engine's extension-point registries.
// Most of those registries store all state in private static maps and only
// expose static methods; the pointer is then "informational" — connectors
// may dereference it to call static methods through the singleton tag, or
// invoke the static methods directly. Either is correct. FunctionMap and
// Logger have real per-instance state and are pointers to their existing
// process-wide singletons.
struct EngineRegistries {
  // ---- Per-instance engine objects ----
  const util::Config*        cfg        { nullptr };
  rt::ThreadPool*            pool       { nullptr };
  AtomicStore*               store      { nullptr };
  Dispatcher*                dispatcher { nullptr };
  rt::ShutdownCoordinator*   shutdown   { nullptr };
  boost::asio::io_context*   io         { nullptr };

  // ---- Engine extension-point registries (singletons) ----
  EventTypeRegistry*         events     { nullptr };
  EventComputerRegistry*     computers  { nullptr };
  NodeTypeRegistry*          nodes      { nullptr };
  IngressRegistry*           ingress    { nullptr };
  ConfigNamespaceRegistry*   configNs   { nullptr };
  AtomicProviderRegistry*    providers  { nullptr };
  FunctionMap*               functions  { nullptr };
  util::Logger*              log        { nullptr };
};

} // namespace gma::engine
