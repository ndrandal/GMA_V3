#pragma once
#include <string>
#include <string_view>

#include "gma/engine/FieldSource.hpp"

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

// Computers materialize derived values (TA, aggregates, etc.) from an inbound
// event. They may write to AtomicStore and/or notify listeners via the
// dispatcher.
//
// COMPUTE TAKES A FieldSource, NOT AN Event (ENC-1486).
// The Dispatcher has two ingress entry points — `onTick` (JSON DOM) and
// `onRecord` (`data::TickRecord`, no DOM) — and SPEC
// specs/2026-10-14-gma-data-layer/SPEC.md D1 requires ONE implementation of
// every operator, computers included. A second `compute` overload taking a
// record would be a second implementation of every connector's field
// extraction, and the two would diverge: a graph could then tell which entry
// point fed it, which is exactly what the record path must never let it do.
//
// So there is one virtual, both entry points call it, and what differs is the
// three-line adapter in front of it. A computer written against a FieldSource
// works identically on JSON and on records with no further work — and cannot
// accidentally support only one.
//
// CONCURRENCY: unchanged. See the CONCURRENCY CONTRACT on
// `Dispatcher::onTick()` — compute() runs outside the dispatcher's computer
// cache lock and may be entered concurrently for the same event type, so
// implementations must be stateless or internally synchronised.
class IEventComputer {
public:
  virtual ~IEventComputer() = default;

  // Name of the event type this computer responds to (e.g. "tick").
  virtual std::string_view eventType() const = 0;

  // THE computer entry point. `symbol` is the stream key — a
  // `const std::string&` and not a `string_view` because every consumer
  // immediately uses it as an `AtomicStore` / `StreamValue` key, both of
  // which take a `std::string`; narrowing it here would only move the copy.
  virtual void compute(const std::string& symbol,
                       const FieldSource& fields,
                       ComputeContext& ctx) = 0;

  // Convenience for a caller that has a JSON `Event` in hand: wraps its
  // payload in a `JsonFieldSource` and calls `compute` above. NOT virtual and
  // NOT what the Dispatcher calls — `onTick` wraps once per tick rather than
  // once per computer. Used by connector unit tests.
  //
  // A null or non-object payload is a no-op, as it was before.
  void computeEvent(const Event& e, ComputeContext& ctx);
};

} // namespace gma::engine
