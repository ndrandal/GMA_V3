// include/gma/AtomicStore.hpp
#pragma once
#include "SymbolValue.hpp"

#include <string>
#include <unordered_map>
#include <variant>
#include <shared_mutex>
#include <optional>
#include <vector>

namespace gma {

class AtomicStore {
public:
  AtomicStore() = default;

  void set(const std::string& symbol, const std::string& field, ArgType value);

  /// Write multiple fields for a symbol under a single lock acquisition.
  void setBatch(const std::string& symbol,
                const std::vector<std::pair<std::string, ArgType>>& fields);

  std::optional<ArgType> get(const std::string& symbol, const std::string& field) const;

private:
  using FieldMap = std::unordered_map<std::string, ArgType>;

  mutable std::shared_mutex _mutex;
  std::unordered_map<std::string, FieldMap> _data;
};

} // namespace gma
