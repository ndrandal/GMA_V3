#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace gma {

// Forward declarations first — ArgType, ArgValue, and RecordField form a
// recursive cycle.
//
// std::vector may be *instantiated* on an incomplete element type, but the
// element must be complete before any member of that specialization is
// referenced ([vector.overview]). Two members below sit on opposite sides of
// that line, and the difference is why this header only ever failed under one
// compiler:
//
//   * ArgType's `std::vector<ArgValue>` alternative is fine. Its members are
//     members of a class template, so their point of instantiation is deferred
//     to the end of the translation unit — by which time ArgValue is complete.
//
//   * Record's `std::vector<RecordField>` member is not. Naming Record as a
//     std::variant alternative makes variant query Record's special member
//     functions; if those are *implicitly declared* they get defined right
//     there, and their definitions reference std::vector<RecordField>'s copy,
//     move and destroy members while RecordField is still incomplete. That is
//     ill-formed (no diagnostic required). libstdc++ under g++ happens to
//     accept it; clang rejects it with 13 errors — see ENC-1073.
//
// The fix is to declare Record's special members here and default them below,
// once RecordField is complete. variant then has real declarations to reason
// about and instantiates nothing early, so no vector<RecordField> member is
// referenced before its element type is complete.
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

  // Declared, not defined: the definitions are further down, after RecordField
  // is complete. Do not replace these with implicit or in-class-defaulted
  // members — that reintroduces the ENC-1073 clang failure described above.
  Record();
  Record(const Record&);
  Record(Record&&) noexcept;
  Record& operator=(const Record&);
  Record& operator=(Record&&) noexcept;
  ~Record();
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

// ----- Record special members (RecordField is complete from here on) -----
//
// std::vector<RecordField>'s members are first referenced here, which is
// exactly what [vector.overview] requires.
inline Record::Record() = default;
inline Record::Record(const Record&) = default;
inline Record::Record(Record&&) noexcept = default;
inline Record& Record::operator=(const Record&) = default;
inline Record& Record::operator=(Record&&) noexcept = default;
inline Record::~Record() = default;

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
