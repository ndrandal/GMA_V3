// src/core/AtomicStore.cpp
#include "gma/AtomicStore.hpp"

namespace gma {

void AtomicStore::set(const std::string& symbol, const std::string& field, ArgType value) {
  std::unique_lock lock(_mutex);
  _data[symbol][field] = std::move(value);
}

void AtomicStore::setBatch(const std::string& symbol,
                           const std::vector<std::pair<std::string, ArgType>>& fields) {
  std::unique_lock lock(_mutex);
  auto& fm = _data[symbol];
  for (const auto& [key, val] : fields) {
    fm[key] = val;
  }
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
