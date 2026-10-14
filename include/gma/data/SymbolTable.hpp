#pragma once
//
// String interning for the record path (ENC-1481).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D3; docs/data-layer.md §5.3.
//
// WHY THIS IS IN gma_engine AND NOT IN gma_data
// ---------------------------------------------
// Interning happens in `gma_data`'s decode — once per distinct symbol per
// batch. But *resolution*, id -> text, happens at an output boundary, and
// every output boundary is engine code: `StreamValue` carries
// `std::string symbol`, so the moment ENC-1486's `Dispatcher::onRecord` turns a
// `TickRecord` into a `StreamValue` it needs the ticker back. The engine cannot
// depend on `gma_data` (SPEC D14), so a table living there would force a second
// implementation of interning on the engine side — the same class of mistake
// D1 refuses for operator math. One table, one definition, in the library both
// halves may legally name.
//
// It is Arrow-free and ClickHouse-free, like Record.hpp beside it.
//
// ID 0 IS RESERVED. `RecordHeader` default-constructs symbolId/venueId to 0, so
// if interning started at 0 a zero-initialized header would name whichever
// string happened to be interned first. Ids start at 1 and 0 is
// kInvalidSymbolId: "this record names no symbol", distinguishable from every
// real one.
//
// This is a general string->id interner, used twice: once for tickers and once
// for venues (docs/data-layer.md §8.3 makes both dictionary columns). It is
// named SymbolTable because the ticker space is the one that matters — a venue
// space has a handful of entries and a symbol space has thousands against
// millions of rows.
//

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gma::data {

using SymbolId = std::uint32_t;

// "Names no symbol." Never returned by intern().
inline constexpr SymbolId kInvalidSymbolId = 0;

class SymbolTable {
 public:
  SymbolTable() = default;

  SymbolTable(const SymbolTable&)            = delete;
  SymbolTable& operator=(const SymbolTable&) = delete;

  // Interns `text` and returns its id, assigning a new one on first sight.
  // Stable for the lifetime of the table: an id is never reused and never
  // changes. Interning "" is not special-cased away — an empty ticker is a
  // provider bug, and silently folding it onto kInvalidSymbolId would hide it;
  // it gets an ordinary id and the caller may reject it.
  SymbolId intern(std::string_view text);

  // Id for `text`, or kInvalidSymbolId if it was never interned. Does not
  // intern — use this where "unknown" is an answer rather than an event.
  SymbolId lookup(std::string_view text) const;

  // Text for `id`, or "" for kInvalidSymbolId and for any id this table never
  // issued. Returned by value: the backing store is stable, but handing out a
  // view would outlive a lock we do not hold for the caller.
  std::string name(SymbolId id) const;

  // Does this table know `id`? Distinguishes "no symbol" and "unknown id"
  // from "a symbol whose text is empty", all three of which name() flattens
  // to "".
  bool contains(SymbolId id) const;

  // Number of interned strings. Ids run 1..size() inclusive.
  std::size_t size() const;

  void clear();

 private:
  // shared_mutex and not a plain mutex because the access pattern is
  // read-mostly and asymmetric: intern() runs once per distinct symbol per
  // batch, name() runs per output value. Deliberately NOT lock-free — this is
  // off the per-record hot path by construction, since a record carries the id
  // and never the text. If a profile ever shows this lock, the fix is to stop
  // resolving per value, not to make the table cleverer.
  //
  // Reader starvation note (ENC-1340): AtomicStore had to add a writer-queued
  // yield because glibc's shared_mutex is reader-preferring. It does not apply
  // here — writes are O(distinct symbols), not O(records), so there is no
  // sustained writer to starve.
  mutable std::shared_mutex mu_;

  // Ids index this vector at (id - 1). `std::string` and not a view: the
  // table owns the text, because the Arrow buffer the ticker arrived in is
  // released when the batch is.
  std::vector<std::string> names_;
  std::unordered_map<std::string, SymbolId> ids_;
};

} // namespace gma::data
