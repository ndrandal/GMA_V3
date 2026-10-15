//
// DbnEncoder — see DbnEncoder.hpp for why this is a library component.
//
// EVERY LINE HERE WAS MOVED, NOT REWRITTEN. It is byte-identical in behaviour
// to the encoder that lived in `DatabentoAdapterTest.cpp` under ENC-1482, with
// exactly one change: the fixed-metadata-block size check was a gtest
// `EXPECT_EQ` and is now a thrown `std::logic_error` plus an out-param, since
// a library cannot link gtest. `DbnEncoderTest.EncoderReproducesEveryFixtureByteForByte`
// still compares this code's output against the committed vendor bytes, so if
// the move had changed a single byte that test would be red.
//

#include "gma/data/DbnEncoder.hpp"

#include <stdexcept>
#include <string>

namespace gma::data::dbn::enc {

void putU8(std::vector<std::byte>& v, std::uint8_t x) { v.push_back(std::byte{x}); }
void putU16(std::vector<std::byte>& v, std::uint16_t x) { putLE(v, x, 2); }
void putU32(std::vector<std::byte>& v, std::uint32_t x) { putLE(v, x, 4); }
void putU64(std::vector<std::byte>& v, std::uint64_t x) { putLE(v, x, 8); }
void putI32(std::vector<std::byte>& v, std::int32_t x) {
  putLE(v, static_cast<std::uint32_t>(x), 4);
}
void putI64(std::vector<std::byte>& v, std::int64_t x) {
  putLE(v, static_cast<std::uint64_t>(x), 8);
}
void putZeros(std::vector<std::byte>& v, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) putU8(v, 0);
}
void putCstr(std::vector<std::byte>& v, const std::string& s, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    putU8(v, i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
  }
}

std::span<const std::byte> asSpan(const std::vector<std::byte>& v) {
  return std::span<const std::byte>(v.data(), v.size());
}

std::size_t DbnWriter::scl() const {
  return version == 1 ? dbn::kSymbolCstrLenV1 : symbolCstrLen;
}

void DbnWriter::addMbo(const dbn::MboMsg& m) {
  putU8(records, static_cast<std::uint8_t>(dbn::kMboMsgLen / dbn::kLengthMultiplier));
  putU8(records, static_cast<std::uint8_t>(dbn::RType::Mbo));
  putU16(records, m.hd.publisherId);
  putU32(records, m.hd.instrumentId);
  putU64(records, m.hd.tsEvent);
  putU64(records, m.orderId);
  putI64(records, m.price);
  putU32(records, m.size);
  putU8(records, m.flags);
  putU8(records, m.channelId);
  putU8(records, static_cast<std::uint8_t>(m.action));
  putU8(records, static_cast<std::uint8_t>(m.side));
  putU64(records, m.tsRecv);
  putI32(records, m.tsInDelta);
  putU32(records, m.sequence);
}

void DbnWriter::addTrade(const dbn::TradeMsg& m) {
  putU8(records, static_cast<std::uint8_t>(dbn::kTradeMsgLen / dbn::kLengthMultiplier));
  putU8(records, static_cast<std::uint8_t>(dbn::RType::Mbp0));
  putU16(records, m.hd.publisherId);
  putU32(records, m.hd.instrumentId);
  putU64(records, m.hd.tsEvent);
  putI64(records, m.price);
  putU32(records, m.size);
  putU8(records, static_cast<std::uint8_t>(m.action));
  putU8(records, static_cast<std::uint8_t>(m.side));
  putU8(records, m.flags);
  putU8(records, m.depth);
  putU64(records, m.tsRecv);
  putI32(records, m.tsInDelta);
  putU32(records, m.sequence);
}

void DbnWriter::addOpaque(dbn::RType rtype, std::uint32_t instrumentId,
                          std::uint64_t tsEvent) {
  putU8(records,
        static_cast<std::uint8_t>(dbn::kRecordHeaderLen / dbn::kLengthMultiplier));
  putU8(records, static_cast<std::uint8_t>(rtype));
  putU16(records, 1);
  putU32(records, instrumentId);
  putU64(records, tsEvent);
}

std::vector<std::byte> DbnWriter::build(std::size_t* fixedBlockLenOut) const {
  std::vector<std::byte> var;
  putU32(var, 0);  // layout_length — always 0 in v1..v3
  const auto repeated = [&](const std::vector<std::string>& xs) {
    putU32(var, static_cast<std::uint32_t>(xs.size()));
    for (const auto& s : xs) putCstr(var, s, scl());
  };
  repeated(symbols);
  repeated(partial);
  repeated(notFound);
  putU32(var, static_cast<std::uint32_t>(mappings.size()));
  for (const auto& m : mappings) {
    putCstr(var, m.rawSymbol, scl());
    putU32(var, static_cast<std::uint32_t>(m.intervals.size()));
    for (const auto& iv : m.intervals) {
      putU32(var, iv.startDate);
      putU32(var, iv.endDate);
      putCstr(var, iv.symbol, scl());
    }
  }

  const std::size_t needed = dbn::kFixedMetadataLen + var.size();
  std::size_t pad = 0;
  if (version >= dbn::kFirstAlignedVersion && needed % dbn::kMetadataAlignment != 0) {
    pad = dbn::kMetadataAlignment - needed % dbn::kMetadataAlignment;
  }

  std::vector<std::byte> out;
  putU8(out, 'D');
  putU8(out, 'B');
  putU8(out, 'N');
  putU8(out, version);
  const std::size_t tail = pad + extraTail;
  putU32(out,
         forceMetadataLen.value_or(static_cast<std::uint32_t>(needed + tail)));

  const std::size_t fixedStart = out.size();
  putCstr(out, dataset, dbn::kDatasetCstrLen);
  putU16(out, schema);
  putU64(out, start);
  putU64(out, end);
  putU64(out, limit);
  if (version == 1) putU64(out, recordCount);
  putU8(out, stypeIn);
  putU8(out, stypeOut);
  putU8(out, tsOut);
  if (version > 1) putU16(out, symbolCstrLen);
  putZeros(out, version == 1 ? dbn::kReservedLenV1 : dbn::kReservedLen);

  const std::size_t fixedLen = out.size() - fixedStart;
  if (fixedBlockLenOut != nullptr) *fixedBlockLenOut = fixedLen;
  if (fixedLen != dbn::kFixedMetadataLen) {
    throw std::logic_error(
        "DbnWriter: the fixed metadata block came out " + std::to_string(fixedLen) +
        " bytes, not " + std::to_string(dbn::kFixedMetadataLen) +
        " — every subsequent byte of this file is at the wrong offset");
  }

  out.insert(out.end(), var.begin(), var.end());
  for (std::size_t i = 0; i < tail; ++i) putU8(out, padByte);
  out.insert(out.end(), records.begin(), records.end());
  return out;
}

}  // namespace gma::data::dbn::enc
