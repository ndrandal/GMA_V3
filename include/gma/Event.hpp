#pragma once

#include <cstdint>
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
//   timestampNs
//             : source-reported event time, in NANOSECONDS SINCE THE UNIX
//               EPOCH. `0` means the source did not report one — it is an
//               absence marker, not a time, and no consumer may treat it as
//               1970-01-01. Trails `type` for the same reason `type` trails
//               `payload`: every existing positional construction keeps
//               working and gets the "not reported" default.
//
//               THE UNIT IS PART OF THE CONTRACT. An ingress that receives a
//               time on some other basis must convert before it gets here;
//               storing a foreign basis in this field is how a timestamp comes
//               to silently mean something other than what it says. ITCH is
//               exactly that case — it stamps nanoseconds since UTC *midnight*
//               — and `gma::feed::ItchAdapter::itchTimestampToEpochNs` is where
//               that conversion happens (ENC-1028).
struct Event {
  std::string                          symbol;
  std::shared_ptr<rapidjson::Document> payload;
  std::string                          type { "tick" };
  std::uint64_t                        timestampNs { 0 };
};
} // namespace gma
