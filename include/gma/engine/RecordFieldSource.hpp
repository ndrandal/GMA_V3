#pragma once
//
// RecordFieldSource — the Arrow-free, DOM-free adapter from a
// `gma::data::TickRecord` to the engine's field view (ENC-1486).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D3, D9;
//         docs/data-layer.md §C2, §C6, §6, §8.3.
//
// THIS HEADER NAMES NO JSON TYPE, AND THAT IS CHECKED.
// `tests/dispatch/RecordIngressParityTest.cpp` asserts it textually, with a
// positive control on `JsonFieldSource.hpp` so the check cannot pass by
// failing to look (the workspace-root-grep failure, in miniature).
//
// NO ALLOCATION PER RECORD. The field set of a `TickRecord` is fixed and
// small, so the mapping is resolved once into a stack `std::array` in the
// constructor and every accessor is a linear scan of at most six entries.
// The names are `inline const std::string` objects with static storage, so
// handing a `const std::string&` to the sink costs nothing — this is the
// `std::string name(it->name.GetString(), ...)` per member per tick that
// docs/data-layer.md §C6 names as the dominant ingress cost, gone.
//
// ------------------------------------------------------------------------
// THE MAPPING. This table *defines* "the equivalent tick" — the JSON Event
// that `Dispatcher::onTick` would have to receive for the two entry points to
// be indistinguishable. The parity test builds its Event from this same
// mapping rather than restating it, so the two cannot drift.
//
//   record field        engine field   emitted when
//   ------------------  -------------  ------------------------------------
//   price               "price"        kind == Trade && price != kNoPrice
//   size                "size"         kind == Trade && size != 0
//   bidPx               "bid"          bidPx != kNoPrice
//   askPx               "ask"          askPx != kNoPrice
//   bidSz               "bidSize"      bidPx != kNoPrice && bidSz != 0
//   askSz               "askSize"      askPx != kNoPrice && askSz != 0
//
// Emission order is the order above, and it is OBSERVABLE IN TWO PLACES. An
// earlier revision of this comment claimed one; an adversarial probe found the
// second, which is the one that matters:
//
//   1. The order of the `AtomicStore::setBatch` the raw-admission ledger
//      produces. Benign on its own — the store is latest-value-wins, so a
//      permutation of one batch leaves identical contents.
//   2. WHICH FIELDS SURVIVE `maxFieldsPerSymbol`. Past the cap the admission
//      ledger refuses NEW fields and the history map refuses new ones, so the
//      surviving set is a prefix of the arrival order. A capped Dispatcher
//      therefore keeps a DIFFERENT set of fields for two tick shapes that
//      differ only in field order — which means "the equivalent tick" is not
//      merely the right names and values, it is the right names and values
//      IN THIS ORDER. Pinned by
//      RecordIngressParityTest.IdenticalWhenTheFieldCapTruncatesAdmission,
//      which also asserts the reverse-order JSON tick does NOT match, so the
//      claim is measured rather than asserted.
//
// Listener fan-out is NOT one of the two: it is ordered by the
// `std::map<field, …>` the Dispatcher keys subscriptions on, so `ask` precedes
// `bid` on both paths regardless of this order, per Dispatcher.hpp's ordering
// contract.
//
// `price`/`size` are gated on `kind == Trade` because `TickRecord` is one
// struct for a trade and an L1 quote and `kind` says which half is
// meaningful (include/gma/data/Record.hpp). The quote half is gated on
// presence only: a record may legitimately carry both a print and the quote
// standing at the time.
//
// WHAT IS DELIBERATELY NOT EXPOSED, and why each absence is a decision:
//
//   * `header.tsEventNs` / `tsRecvNs`. Event time is SPEC D9 and
//     docs/data-layer.md §C1 — explicitly post-measurement and not this
//     slice. Exposing the venue timestamp as a numeric field would create a
//     de-facto event-time channel for operators to key off before the
//     decision that governs it has been implemented, and it would do so
//     LOSSILY: a nanosecond epoch exceeds 2^53 and does not survive the
//     `double` the engine's value path carries.
//   * `providerSeq`, `sessionId`, `instrumentId`, `venueId`, `tradeId`.
//     Provenance, not values. docs/data-layer.md §C3 records that the engine
//     value path has no sequence number; adding one here would be that
//     change, made sideways.
//   * `kind`, `aggressor`, `bookNeutral`, `flags`. Categorical. The engine's
//     admitted-field path is a `double` series per `(symbol, field)`; a
//     categorical value needs a typed channel, which is not this slice.
//
// Adding a row to the table above is a behaviour change on BOTH paths: the
// parity test derives the equivalent Event from it, so a new field is
// admitted, historied and subscribable on the record path and must be made
// so on the JSON path too, or parity fails. That is the intended friction.
//

#include <array>
#include <cstdint>
#include <string>

#include "gma/data/Record.hpp"
#include "gma/engine/FieldSource.hpp"

namespace gma::engine {

// The engine-visible names of a TickRecord's fields. `inline const` (not
// function-local statics) so there is no thread-safe-init guard on the hot
// path, and so the spelling exists exactly once.
namespace recordfields {
inline const std::string kPrice   = "price";
inline const std::string kSize    = "size";
inline const std::string kBid     = "bid";
inline const std::string kAsk     = "ask";
inline const std::string kBidSize = "bidSize";
inline const std::string kAskSize = "askSize";
}  // namespace recordfields

class RecordFieldSource final : public FieldSource {
public:
  // Largest possible emitted field count — see the mapping table.
  static constexpr std::size_t kMaxFields = 6;

  explicit RecordFieldSource(const data::TickRecord& rec) noexcept {
    if (rec.kind == data::RecordKind::Trade) {
      if (rec.price != data::kNoPrice) {
        addPrice(recordfields::kPrice, rec.price);
      }
      if (rec.size != 0) {
        addCount(recordfields::kSize, rec.size);
      }
    }
    if (rec.bidPx != data::kNoPrice) {
      addPrice(recordfields::kBid, rec.bidPx);
    }
    if (rec.askPx != data::kNoPrice) {
      addPrice(recordfields::kAsk, rec.askPx);
    }
    if (rec.bidPx != data::kNoPrice && rec.bidSz != 0) {
      addCount(recordfields::kBidSize, rec.bidSz);
    }
    if (rec.askPx != data::kNoPrice && rec.askSz != 0) {
      addCount(recordfields::kAskSize, rec.askSz);
    }
  }

  bool isObject() const noexcept override { return true; }

  std::size_t fieldCountHint() const noexcept override { return n_; }

  bool has(const std::string& name) const override {
    return find(name) != nullptr;
  }

  bool tryGetNumber(const std::string& name, double& out) const override {
    const Entry* e = find(name);
    if (!e) return false;
    out = e->value;
    return true;
  }

  // ALWAYS FALSE, and that is a parity decision rather than a shortcut.
  //
  // `tryGetUint64` exists for exactly one caller: `MarketTickComputer` reads
  // `MarketFieldMap::timestampField` as an exact `u64`, because a nanosecond
  // epoch does not survive a `double`. No field this source exposes has
  // exact-integer identity — the record path carries no timestamp at all
  // (SPEC D9: event time is post-measurement), and the size columns are
  // exposed as doubles because that is what the engine's value path is.
  //
  // Answering `true` for a size column would be a genuine divergence between
  // the two entry points: rapidjson's `IsUint64()` is false for a value
  // stored as a double, so a field map configured with
  // `timestampField = "size"` would read a timestamp on the record path and
  // not on the JSON one. Refusing here keeps the two identical for EVERY
  // field map, not just the default. Pinned by
  // RecordIngressParityTest.ParityHoldsWithATimestampFieldConfigured.
  bool tryGetUint64(const std::string&, std::uint64_t&) const override {
    return false;
  }

  void forEachNumber(NumericFieldSink& sink) const override {
    for (std::size_t i = 0; i < n_; ++i) {
      sink.field(*fields_[i].name, fields_[i].value);
    }
  }

private:
  struct Entry {
    const std::string* name  = nullptr;
    double             value = 0.0;
  };

  // `name` MUST have static storage duration — an `Entry` keeps a POINTER to
  // it, which is what makes handing a `const std::string&` to the sink free.
  // Every caller passes a `recordfields::` object; a temporary here would
  // dangle the moment the constructor returned.
  void addPrice(const std::string& name, std::int64_t fixed) noexcept {
    fields_[n_++] = Entry{&name, data::toDouble(fixed)};
  }

  void addCount(const std::string& name, std::uint64_t count) noexcept {
    fields_[n_++] = Entry{&name, static_cast<double>(count)};
  }

  const Entry* find(const std::string& name) const noexcept {
    for (std::size_t i = 0; i < n_; ++i) {
      if (*fields_[i].name == name) return &fields_[i];
    }
    return nullptr;
  }

  std::array<Entry, kMaxFields> fields_{};
  std::size_t                   n_ = 0;
};

} // namespace gma::engine
