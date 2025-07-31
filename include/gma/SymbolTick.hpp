#pragma once

#include <string>
#include <memory>               // for shared_ptr
#include <rapidjson/document.h>

namespace gma {
struct SymbolTick {
  std::string                               symbol;   // e.g. "AAPL"
  std::shared_ptr<rapidjson::Document>      payload;  // full DOM, owned by shared_ptr
};
} // namespace gma
