#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace gma {

struct TickEntry {
  double   price;
  double   volume;
  double   bid = 0.0;          // best bid (0 = not provided)
  double   ask = 0.0;          // best ask (0 = not provided)
  uint64_t timestampNs = 0;    // nanoseconds since epoch (0 = not provided)
};

using SymbolHistory = std::deque<TickEntry>;

} // namespace gma
