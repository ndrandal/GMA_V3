#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gma {

// Forward declarations first — ArgType, ArgValue, and RecordField form a
// recursive cycle. The cycle is broken exactly the way the standard guarantees
// for std::vector: a vector member may name an incomplete element type, so
// Record can hold std::vector<RecordField> before RecordField is complete, and
// ArgType's std::vector<ArgValue> can name ArgValue before it is complete.
struct ArgValue;
struct RecordField;

// Keyed record/map value (ENC-642): an insertion-ordered set of named fields —
// the structured value type that lets one pipeline carry e.g. an OHLC candle
// instead of N parallel scalar pipelines re-joined downstream. Records are
// small by construction (a handful of fields), so lookup is linear. Insertion
// order is preserved so serialization is deterministic and mirrors the order
// the fields were packed.
struct Record {
  std::vector<RecordField> fields;
};

// ArgType: the value carried on every pipeline edge. `std::vector<ArgValue>` is
// a positional (heterogeneous) array; `Record` is a keyed object.
using ArgType = std::variant<
  bool,
  int,
  double,
  std::string,
  std::vector<int>,
  std::vector<double>,
  std::vector<ArgValue>,
  Record
>;

// Wrapper that closes the recursion: an ArgValue *is* an ArgType, letting
// arrays/records nest arbitrarily.
struct ArgValue {
  ArgType value;

  ArgValue() = default;
  ArgValue(const ArgType& val) : value(val) {}
  ArgValue(ArgType&& val) : value(std::move(val)) {}

  // Enable implicit conversion from any ArgType alternative.
  template <typename T,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, ArgValue>>>
  ArgValue(T&& val) : value(std::forward<T>(val)) {}
};

// One named field of a Record. Defined after ArgValue so its ArgValue member is
// complete here.
struct RecordField {
  std::string name;
  ArgValue    value;
};

// ----- Record helpers (ArgValue/RecordField are complete from here on) -----

// Returns a pointer to the named field's value, or nullptr if absent.
inline const ArgType* recordFind(const Record& r, const std::string& key) {
  for (const auto& f : r.fields)
    if (f.name == key) return &f.value.value;
  return nullptr;
}

// Inserts or overwrites a field. On overwrite the field keeps its original
// position; new fields are appended (insertion order preserved).
inline void recordSet(Record& r, const std::string& key, ArgType val) {
  for (auto& f : r.fields)
    if (f.name == key) { f.value.value = std::move(val); return; }
  r.fields.push_back(RecordField{key, ArgValue{std::move(val)}});
}

// Core value for computation — carries a stream-key + computed value through the
// node pipeline.
struct StreamValue {
  std::string symbol;
  ArgType value;
};

} // namespace gma
