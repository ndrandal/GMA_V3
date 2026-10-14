#include "gma/data/SymbolTable.hpp"

#include "gma/data/Record.hpp"

#include <cmath>
#include <mutex>

namespace gma::data {

SymbolId SymbolTable::intern(std::string_view text) {
  // Fast path: already present. Taken under a shared lock so concurrent
  // decoders of the same symbol do not serialize on the exclusive one.
  {
    std::shared_lock lk(mu_);
    auto it = ids_.find(std::string(text));
    if (it != ids_.end()) return it->second;
  }

  std::unique_lock lk(mu_);
  // Re-check: another thread may have interned it between the two locks.
  auto it = ids_.find(std::string(text));
  if (it != ids_.end()) return it->second;

  names_.emplace_back(text);
  const auto id = static_cast<SymbolId>(names_.size());  // 1-based; 0 reserved
  ids_.emplace(names_.back(), id);
  return id;
}

SymbolId SymbolTable::lookup(std::string_view text) const {
  std::shared_lock lk(mu_);
  auto it = ids_.find(std::string(text));
  return it == ids_.end() ? kInvalidSymbolId : it->second;
}

std::string SymbolTable::name(SymbolId id) const {
  std::shared_lock lk(mu_);
  if (id == kInvalidSymbolId || id > names_.size()) return {};
  return names_[id - 1];
}

bool SymbolTable::contains(SymbolId id) const {
  std::shared_lock lk(mu_);
  return id != kInvalidSymbolId && id <= names_.size();
}

std::size_t SymbolTable::size() const {
  std::shared_lock lk(mu_);
  return names_.size();
}

void SymbolTable::clear() {
  std::unique_lock lk(mu_);
  names_.clear();
  ids_.clear();
}

// ---------------------------------------------------------------------------
// Record.hpp's one out-of-line function lives here rather than in its own
// translation unit: it is two lines and Record.hpp is otherwise header-only.
// ---------------------------------------------------------------------------

std::int64_t toFixed(double px) noexcept {
  // Rounds half away from zero, matching std::llround. Providers that hand
  // over fixed point (Databento DBN is already 1e-9) must never come through
  // here — SPEC D8's "exact integer path with no double round trip" is about
  // precisely this function not being on the DBN path.
  return static_cast<std::int64_t>(std::llround(px * static_cast<double>(kPriceScale)));
}

} // namespace gma::data
