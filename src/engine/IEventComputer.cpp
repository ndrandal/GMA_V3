#include "gma/engine/IEventComputer.hpp"

#include "gma/Event.hpp"
#include "gma/engine/JsonFieldSource.hpp"

namespace gma::engine {

// The JSON DOM adapter. Lives in a .cpp so `IEventComputer.hpp` — which every
// connector includes — stays free of rapidjson.
void IEventComputer::computeEvent(const Event& e, ComputeContext& ctx) {
  if (!e.payload) return;
  const JsonFieldSource fields(*e.payload);
  compute(e.symbol, fields, ctx);
}

} // namespace gma::engine
