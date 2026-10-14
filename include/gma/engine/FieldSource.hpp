#pragma once
//
// FieldSource — one tick's resolved numeric fields, independent of how they
// arrived (ENC-1486).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D3;
//         docs/data-layer.md §C2, §C6, §6.
//
// WHY THIS TYPE EXISTS
// --------------------
// `Dispatcher` has two ingress entry points, and must have exactly one body:
//
//   Dispatcher::onTick  (const Event&)             — JSON DOM, unchanged
//   Dispatcher::onRecord(const data::TickRecord&)  — resolved fields, no DOM
//
// SPEC D1 is "one implementation of every operator", and the Dispatcher's
// routing, history, atomic-key and listener-ordering rules are as much an
// operator as `mean` is: two copies of them would diverge, and a graph would
// then be able to tell which entry point fed it. So the two entry points are
// *adapters* that differ only in how a field is read, and everything after
// that — raw admission, history, `computeAndStoreAtomics`, `deliver` order —
// is one function taking a `const FieldSource&`.
//
// The only virtual dispatch is the per-field read. That is deliberate: the
// cost this interface exists to remove is a rapidjson DOM plus a
// `std::string` construction per member per tick (docs/data-layer.md §C6),
// which is orders of magnitude above a virtual call.
//
// NON-OWNING. A FieldSource is a view over something else's storage — a
// `rapidjson::Document`, a `data::TickRecord` — and is valid only for the
// duration of one ingress call. Never store one.
//
// NUMERIC ONLY, BY DESIGN. The engine's value path is `double` (and,
// through `ArgType`, `int`/`bool`/strings — but `Dispatcher` admits and
// histories only numbers). `tryGetUint64` exists because
// `MarketTickComputer` reads a nanosecond timestamp, which does not survive
// a `double`.
//

#include <cstdint>
#include <string>

namespace gma::engine {

// Receives the numeric fields of one tick, in the source's natural order.
// An abstract sink rather than a `std::function`: the Dispatcher's sink
// captures three pointers, which is past libstdc++'s `std::function` small-
// object buffer, so a `std::function` here would heap-allocate once per tick
// on the path whose whole purpose is removing a heap allocation per tick.
class NumericFieldSink {
public:
  virtual ~NumericFieldSink() = default;
  virtual void field(const std::string& name, double value) = 0;
};

class FieldSource {
public:
  virtual ~FieldSource() = default;

  // False when the underlying payload is not a field container at all (a JSON
  // scalar or array). Every accessor below is well-defined and empty in that
  // state; it is never an abort and never undefined.
  virtual bool isObject() const noexcept = 0;

  // Upper bound on the number of fields, for reserve(). Not necessarily the
  // number the sink will see — the JSON source counts non-numeric members too.
  virtual std::size_t fieldCountHint() const noexcept = 0;

  // Is `name` present AT ALL — numeric or not? This is the predicate the
  // listener fan-out has always used (`payload->HasMember(field)`), kept
  // distinct from `tryGetNumber` because the two differ for a present
  // non-numeric field and `onTick` has always treated that case as
  // "subscribed, but no value this tick".
  virtual bool has(const std::string& name) const = 0;

  // Reads `name` as a double. False if absent or not a number; `out` is then
  // untouched.
  virtual bool tryGetNumber(const std::string& name, double& out) const = 0;

  // Reads `name` as an exact unsigned 64-bit integer. False if absent, not a
  // number, or not representable as one — matching rapidjson's `IsUint64()`,
  // which is what the only caller used before this type existed.
  virtual bool tryGetUint64(const std::string& name, std::uint64_t& out) const = 0;

  // Hands every NUMERIC field to `sink`, in the source's natural order.
  // Non-numeric fields are skipped, exactly as the pre-ENC-1486 raw-admission
  // loop skipped them.
  //
  // THE NAME REFERENCE IS VALID ONLY FOR THE DURATION OF THE CALL. The JSON
  // source reuses one buffer across members, so a sink that stores the
  // reference stores a name that is about to change. Copy it if you keep it.
  virtual void forEachNumber(NumericFieldSink& sink) const = 0;
};

} // namespace gma::engine
