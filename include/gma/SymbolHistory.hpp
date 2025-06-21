#pragma once

#include <deque>
#include <string>

namespace gma {

struct TickEntry {
  double price;
  double volume;
};

using SymbolHistory = std::deque<TickEntry>;

} // namespace gma
