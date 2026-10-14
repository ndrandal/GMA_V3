// include/gma/AtomicStore.hpp
#pragma once
#include "StreamValue.hpp"

#include <atomic>
#include <string>
#include <unordered_map>
#include <variant>
#include <shared_mutex>
#include <algorithm>
#include <optional>
#include <tuple>
#include <vector>

namespace gma {

class AtomicStore {
public:
  AtomicStore() = default;

  /// Configure cap enforcement. 0 means unlimited; defaults are unlimited.
  /// Apply once at boot from the engine Config; thread-safe but expected to
  /// be a single up-front call before any writes.
  void setCaps(std::size_t maxStreamKeys, std::size_t maxFieldsPerStreamKey);

  /// Write a single field. Silently dropped if a cap would be exceeded —
  /// only-on-new-key/new-field; existing entries are always updatable.
  void set(const std::string& streamKey, const std::string& field, ArgType value);

  /// Write multiple fields for a streamKey under a single lock acquisition.
  /// Same cap semantics as set(): drops only the entries that would push past
  /// a cap; existing entries always update.
  void setBatch(const std::string& streamKey,
                const std::vector<std::pair<std::string, ArgType>>& fields);

  std::optional<ArgType> get(const std::string& streamKey, const std::string& field) const;

  /// ENC-1486 — DIAGNOSTIC FULL ENUMERATION of everything the store holds, as
  /// `(streamKey, field, value)` triples sorted by key then field.
  ///
  /// Read it as an observability accessor, exactly like
  /// `Dispatcher::listenersFor` / `subscriptionCount()`: nothing in `src/`
  /// calls it, it is NOT a synchronisation point, and the returned vector is a
  /// snapshot that may be stale the instant it returns. Do not build routing
  /// or caching on it.
  ///
  /// WHY IT EXISTS. ENC-1486's parity test has to assert that
  /// `Dispatcher::onRecord` and `Dispatcher::onTick` leave IDENTICAL store
  /// contents. With no enumeration accessor the only available comparison was
  /// over a candidate key list the test derived from `FunctionMap`,
  /// `computeAllAtomicValues` and the record→field mapping — and an
  /// adversarial probe showed exactly what that misses: a one-line
  /// `_store->set(symbol, "zzGhost", 1.0)` on the record path only was
  /// INVISIBLE to the comparison, because `zzGhost` was not a name the test
  /// knew to ask for. A parity assertion that can only see keys it already
  /// guessed is not a parity assertion. This makes the comparison total.
  ///
  /// Deliberately sorted and deliberately by value: `_data` is an
  /// `unordered_map`, so iteration order is an implementation detail and two
  /// stores holding the same contents must compare equal regardless of
  /// insertion history.
  std::vector<std::tuple<std::string, std::string, ArgType>> snapshot() const;

private:
  using FieldMap = std::unordered_map<std::string, ArgType>;

  mutable std::shared_mutex _mutex;
  // ENC-1340: writer-starvation guard. `std::shared_mutex` on libstdc++ is a
  // `pthread_rwlock_t` with DEFAULT attributes — PTHREAD_RWLOCK_PREFER_READER_NP
  // — so an arriving reader may join a live shared acquisition even while a
  // writer is already blocked. A reader population that never lets the shared
  // count reach zero starves that writer for as long as it keeps reading, with
  // no bound. Writers publish here before they block and an arriving reader
  // stands down (bounded) while it is non-zero, so the count can drain. Read
  // the full argument, and why this is a HINT rather than a fair lock, in
  // src/core/AtomicStore.cpp.
  std::atomic<unsigned> _writersQueued{0};
  std::unordered_map<std::string, FieldMap> _data;
  std::size_t _maxStreamKeys{0};         // 0 = unlimited
  std::size_t _maxFieldsPerStreamKey{0}; // 0 = unlimited
};

} // namespace gma
