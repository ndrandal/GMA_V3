#pragma once
#include <string_view>

namespace gma {
struct Event;         // gma/Event.hpp
class AtomicStore;    // gma/AtomicStore.hpp
class Dispatcher;     // gma/Dispatcher.hpp
namespace rt { class ThreadPool; }
}

namespace gma::engine {

// Invocation context passed to every IEventComputer::compute() call.
// ComputeContext is deliberately lightweight so connectors can mock it in unit tests.
struct ComputeContext {
  AtomicStore*    store      { nullptr };
  Dispatcher*     dispatcher { nullptr };
  rt::ThreadPool* pool       { nullptr };
};

// Computers materialize derived values (TA, aggregates, etc.) from an inbound Event.
// They may write to AtomicStore and/or notify listeners via the dispatcher.
class IEventComputer {
public:
  virtual ~IEventComputer() = default;

  // Name of the event type this computer responds to (e.g. "tick").
  virtual std::string_view eventType() const = 0;

  virtual void compute(const Event& e, ComputeContext& ctx) = 0;
};

} // namespace gma::engine
