// src/core/AtomicStore.cpp
#include "gma/AtomicStore.hpp"
#include <mutex>

namespace gma {

void AtomicStore::set(const std::string& streamKey, const std::string& field, ArgType value) {
  std::unique_lock lock(_mutex);
  _data[streamKey][field] = std::move(value);
}

void AtomicStore::setBatch(const std::string& streamKey,
                           const std::vector<std::pair<std::string, ArgType>>& fields) {
  std::unique_lock lock(_mutex);
  auto& fm = _data[streamKey];
  for (const auto& [key, val] : fields) {
    fm[key] = val;
  }
}

std::optional<ArgType> AtomicStore::get(const std::string& streamKey, const std::string& field) const {
  std::shared_lock lock(_mutex);

  auto skIt = _data.find(streamKey);
  if (skIt == _data.end()) return std::nullopt;

  const auto& fieldMap = skIt->second;
  auto fieldIt = fieldMap.find(field);
  if (fieldIt == fieldMap.end()) return std::nullopt;

  return fieldIt->second;
}

} // namespace gma
