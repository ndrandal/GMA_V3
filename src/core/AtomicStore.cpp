// src/core/AtomicStore.cpp
#include "gma/AtomicStore.hpp"

namespace gma {

void AtomicStore::set(const std::string& symbol, const std::string& field, ArgType value) {
  std::unique_lock lock(_mutex);
  _data[symbol][field] = std::move(value);
}

std::optional<ArgType> AtomicStore::get(const std::string& symbol, const std::string& field) const {
  std::shared_lock lock(_mutex);

  auto symIt = _data.find(symbol);
  if (symIt == _data.end()) return std::nullopt;

  const auto& fieldMap = symIt->second;
  auto fieldIt = fieldMap.find(field);
  if (fieldIt == fieldMap.end()) return std::nullopt;

  return fieldIt->second;
}

} // namespace gma
