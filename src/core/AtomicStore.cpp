// src/core/AtomicStore.cpp
#include "gma/AtomicStore.hpp"
#include <mutex>

namespace gma {

void AtomicStore::setCaps(std::size_t maxStreamKeys, std::size_t maxFieldsPerStreamKey) {
  std::unique_lock lock(_mutex);
  _maxStreamKeys         = maxStreamKeys;
  _maxFieldsPerStreamKey = maxFieldsPerStreamKey;
}

void AtomicStore::set(const std::string& streamKey, const std::string& field, ArgType value) {
  std::unique_lock lock(_mutex);

  auto skIt = _data.find(streamKey);
  if (skIt == _data.end()) {
    if (_maxStreamKeys > 0 && _data.size() >= _maxStreamKeys) {
      // Cap reached — drop the write rather than evict.
      return;
    }
    skIt = _data.emplace(streamKey, FieldMap{}).first;
  }

  auto& fm = skIt->second;
  if (!fm.contains(field)) {
    if (_maxFieldsPerStreamKey > 0 && fm.size() >= _maxFieldsPerStreamKey) {
      return;
    }
  }
  fm[field] = std::move(value);
}

void AtomicStore::setBatch(const std::string& streamKey,
                           const std::vector<std::pair<std::string, ArgType>>& fields) {
  std::unique_lock lock(_mutex);

  auto skIt = _data.find(streamKey);
  if (skIt == _data.end()) {
    if (_maxStreamKeys > 0 && _data.size() >= _maxStreamKeys) {
      return;
    }
    skIt = _data.emplace(streamKey, FieldMap{}).first;
  }
  auto& fm = skIt->second;

  for (const auto& [key, val] : fields) {
    if (!fm.contains(key)) {
      if (_maxFieldsPerStreamKey > 0 && fm.size() >= _maxFieldsPerStreamKey) {
        continue;
      }
    }
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
