#pragma once

#include <string>
#include <memory>               // for shared_ptr
#include <rapidjson/document.h>

namespace gma {
// Canonical ingress event — connector-agnostic.
//   symbol    : stream key (e.g. "AAPL"). The engine stores this verbatim and
//               never interprets it; connectors choose the key convention.
//   payload   : full JSON DOM, shared-ptr-owned so slow subscribers don't copy.
//   type      : event-type name used by Dispatcher to route to matching
//               IEventComputer implementations. Trails the legacy fields so
//               existing `Event{sym, payload}` positional constructions keep
//               working and implicitly pick up the default "tick" type.
struct Event {
  std::string                          symbol;
  std::shared_ptr<rapidjson::Document> payload;
  std::string                          type { "tick" };
};
} // namespace gma
