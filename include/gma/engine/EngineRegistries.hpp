#pragma once

namespace boost::asio { class io_context; }

namespace gma {
class AtomicStore;
class Dispatcher;
namespace rt   { class ThreadPool; class ShutdownCoordinator; }
namespace util { struct Config; }
}

namespace gma::engine {

// Handle passed to every IConnector::registerWith() call. Bundles the
// long-lived engine pieces a connector may need during boot: config, thread
// pool, store, dispatcher, shutdown coordinator, and the ASIO io_context used
// for socket work. Static registries (NodeTypeRegistry, FunctionMap, etc.) are
// accessed through their own singletons and not re-exposed here.
struct EngineRegistries {
  const util::Config*        cfg        { nullptr };
  rt::ThreadPool*            pool       { nullptr };
  AtomicStore*               store      { nullptr };
  Dispatcher*          dispatcher { nullptr };
  rt::ShutdownCoordinator*   shutdown   { nullptr };
  boost::asio::io_context*   io         { nullptr };
};

} // namespace gma::engine
