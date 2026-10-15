#pragma once
//
// A minimal DBN encoder — promoted out of DatabentoAdapterTest.cpp (ENC-1498).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D6, D11; docs/data-layer.md
//         §8.6, §8.7, §14.2.
//
// WHY THIS IS A LIBRARY COMPONENT AND NOT TEST-LOCAL ANY MORE
// -----------------------------------------------------------
// It was written for ENC-1482 as a non-circularity instrument: the DBN layout
// claims in `DatabentoAdapter.hpp` are believed only because an INDEPENDENTLY
// WRITTEN encoder, fed the values we believe the five committed vendor
// fixtures hold, reproduces each of those files BYTE FOR BYTE
// (`DbnEncoderTest.EncoderReproducesEveryFixtureByteForByte`).
//
// ENC-1498 needs to author a synthetic corpus in REAL DBN, and the one thing
// it must not do is hand-roll DBN bytes: bytes we invented would make every
// format conclusion in this repo circular again, which is exactly what
// ENC-1482 was built to avoid. So the encoder moves here, where a tool can
// link it, and the byte-for-byte test KEEPS COMPARING THIS CODE against the
// committed vendor bytes read off disk. The property that makes the synthetic
// corpus structurally trustworthy is therefore the same property, asserted
// against the same files, about the same code — not about a copy of it.
//
// Concretely: after this promotion `DatabentoAdapterTest.cpp` holds NO encoder
// of its own. Its `fixtureWriter()` configures *this* `DbnWriter`, and the
// comparison target is still `readFixture("mbo.v3.dbn")` et al. If the two had
// been allowed to diverge — a copy here, a copy there — the gate would prove a
// copy and the corpus would be authored by something unproven.
//
// IT IS AN ENCODER, NOT A VALIDATOR. It writes what it is told, including
// deliberately malformed streams: `forceMetadataLen`, `extraTail` and
// `padByte` exist so the adapter's refusal paths can be driven, and
// `addOpaque` writes a header-only record of an arbitrary declared rtype. A
// caller that wants well-formed output must supply well-formed values. The one
// thing `build()` refuses to get wrong silently is its own fixed-metadata
// block size — see below.
//
// NO GTEST, NO ARROW. This header pulls in neither; it is bytes in, bytes out,
// so the synthetic-corpus tool, the ingest path and the test suite all share
// one encoder without any of them acquiring the others' dependencies.
//
// MIRRORS `databento/dbn`'s own encoder (`rust/dbn/src/encode/dbn/sync.rs`)
// for the parts v1..v3 metadata and the `mbo`/`trades` record bodies need.
//

#include "gma/data/DatabentoAdapter.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gma::data::dbn::enc {

// ===========================================================================
// Byte helpers — explicit little-endian stores.
//
// Deliberately NOT `memcpy` of a struct: the wire layout must be a documented
// fact rather than an accident of the compiler's padding and the host's
// endianness, which is the same reason `dbn::MboMsg` is decoded field by field
// rather than `reinterpret_cast` from the buffer.
// ===========================================================================

void putU8(std::vector<std::byte>& v, std::uint8_t x);

template <typename T>
void putLE(std::vector<std::byte>& v, T x, std::size_t n) {
  const auto u = static_cast<std::uint64_t>(x);
  for (std::size_t i = 0; i < n; ++i) {
    putU8(v, static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
  }
}

void putU16(std::vector<std::byte>& v, std::uint16_t x);
void putU32(std::vector<std::byte>& v, std::uint32_t x);
void putU64(std::vector<std::byte>& v, std::uint64_t x);
void putI32(std::vector<std::byte>& v, std::int32_t x);
void putI64(std::vector<std::byte>& v, std::int64_t x);
void putZeros(std::vector<std::byte>& v, std::size_t n);
// A fixed-width NUL-padded field, exactly as DBN writes one.
void putCstr(std::vector<std::byte>& v, const std::string& s, std::size_t width);

std::span<const std::byte> asSpan(const std::vector<std::byte>& v);

// ===========================================================================
// Symbology, as DBN's variable metadata section carries it.
// ===========================================================================

struct Interval {
  std::uint32_t startDate;  // yyyymmdd; HALF-OPEN [startDate, endDate)
  std::uint32_t endDate;
  std::string   symbol;
};
struct Mapping {
  std::string           rawSymbol;
  std::vector<Interval> intervals;
};

// ===========================================================================
// DbnWriter
// ===========================================================================

struct DbnWriter {
  std::uint8_t  version       = 2;
  std::string   dataset       = "GLBX.MDP3";
  std::uint16_t schema        = 0;                 // 0xFFFF == mixed/null
  std::uint64_t start         = 1609160400000000000ULL;
  std::uint64_t end           = 1609200000000000000ULL;
  std::uint64_t limit         = 0;
  std::uint64_t recordCount   = dbn::kNullRecordCount;  // v1 only
  std::uint8_t  stypeIn       = static_cast<std::uint8_t>(dbn::SType::RawSymbol);
  std::uint8_t  stypeOut      = static_cast<std::uint8_t>(dbn::SType::InstrumentId);
  std::uint8_t  tsOut         = 0;
  std::uint16_t symbolCstrLen = 71;
  std::vector<std::string> symbols  = {"ESH1"};
  std::vector<std::string> partial;
  std::vector<std::string> notFound;
  std::vector<Mapping>     mappings = {
      Mapping{"ESH1", {Interval{20201228, 20201229, "5482"}}}};
  // Raw record bytes, appended by the helpers below.
  std::vector<std::byte> records;
  // Set to truncate/corrupt the declared metadata length for a refusal test.
  std::optional<std::uint32_t> forceMetadataLen;
  // Extra metadata tail bytes beyond the natural v3 alignment padding, and the
  // byte value the whole tail is filled with. Both default to "exactly what a
  // real encoder writes"; the refusal tests move them.
  std::size_t  extraTail = 0;
  std::uint8_t padByte   = 0;

  // The symbol-cstr width this version writes. v1 has no `symbol_cstr_len`
  // field in its metadata and uses a fixed 22.
  std::size_t scl() const;

  void addMbo(const dbn::MboMsg& m);
  void addTrade(const dbn::TradeMsg& m);
  // A header-only record of some other declared rtype, for the
  // "every declared rtype is accepted" test.
  void addOpaque(dbn::RType rtype, std::uint32_t instrumentId,
                 std::uint64_t tsEvent);

  // Encode the whole file: prefix, metadata (fixed + variable + v3 alignment
  // padding), then the accumulated records.
  //
  // `fixedBlockLenOut`, when non-null, receives the number of bytes actually
  // written into the FIXED metadata block. `build()` additionally THROWS
  // `std::logic_error` if that is not `dbn::kFixedMetadataLen`, which is the
  // one invariant it will not get wrong quietly: the fixed block is 100 bytes
  // in every version and what fills it differs per version, so a per-version
  // field list that does not add up would shift every subsequent byte — the
  // same failure mode as reading records at a 7-byte offset. In the test
  // translation unit this was a gtest `EXPECT_EQ`; a library cannot use one,
  // and downgrading it to a comment would have deleted the check.
  std::vector<std::byte> build(std::size_t* fixedBlockLenOut = nullptr) const;
};

}  // namespace gma::data::dbn::enc
