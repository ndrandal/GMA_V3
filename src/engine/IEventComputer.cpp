#include "gma/engine/IEventComputer.hpp"

#include "gma/Event.hpp"
#include "gma/engine/JsonFieldSource.hpp"

namespace gma::engine {

// The JSON DOM adapter. Lives in a .cpp so `IEventComputer.hpp` — which every
// connector includes — stays free of rapidjson.
void IEventComputer::computeEvent(const Event& e, ComputeContext& ctx) {
  // A null payload is an EMPTY field set, not a skipped call — see the
  // null-accepting JsonFieldSource ctor for why that distinction matters.
  const JsonFieldSource fields(e.payload.get());
  compute(e.symbol, fields, ctx);
}

} // namespace gma::engine
