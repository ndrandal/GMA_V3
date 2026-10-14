//
// DatabentoAdapter — ENC-1482.
//
// HOW THIS SUITE ESTABLISHES ITS FACTS, WHICH IS THE POINT OF IT
// -------------------------------------------------------------
// The DBN record layout and field names were listed as UNVERIFIED in
// docs/data-layer.md §8.9 ("from memory"). This suite is where they stop being
// that, and the order of the claims matters:
//
//   1. `data/tests/fixtures/dbn/` holds five files of REAL Databento DBN bytes,
//      copied from `databento-cpp`'s own Apache-2.0 test data (provenance and
//      licensing: `PROVENANCE.md` beside them). Nobody here wrote those bytes.
//   2. `DbnEncoderTest.EncoderReproducesEveryFixtureByteForByte` asserts that a
//      minimal DBN encoder living in this file, fed the values we BELIEVE those
//      files contain, reproduces each of them BYTE FOR BYTE. One assertion,
//      three facts: the layout is right, the believed values are right, and
//      the encoder is right.
//   3. Only then is the encoder used to build streams covering the rest of the
//      action and side vocabulary, which the two-record public samples cannot
//      reach (they contain one action, 'C', and one side, 'A').
//
// Without (2), step (3) would be circular — bytes we invented confirming a
// layout we invented. With it, the encoder is an instrument validated against
// vendor data, and the expectations have a second independent source:
// `databento-cpp`'s `DbnDecoderSchemaTests` asserts the same numbers against
// the same files.
//
// ACCEPTANCE IS TESTED, NOT ONLY REJECTION. Every declared `Action`, `Side` and
// `RType` enumerator is asserted to be ACCEPTED, enumerated from
// `dbn::kAllActions` / `kAllSides` / `kAllRTypes` rather than spot-checked —
// a suite that only tests rejection cannot detect a bound that over-rejects a
// now-legal value, which is the bug ENC-1481 shipped and fixed.
//

#include "gma/data/DatabentoAdapter.hpp"

#include <arrow/array.h>
#include <arrow/record_batch.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef GMA_DATA_DBN_FIXTURE_DIR
#error "GMA_DATA_DBN_FIXTURE_DIR must be defined by the build (data/CMakeLists.txt)"
#endif

namespace gma::data {
namespace {

// ===========================================================================
// Byte helpers
// ===========================================================================

void putU8(std::vector<std::byte>& v, std::uint8_t x) { v.push_back(std::byte{x}); }

template <typename T>
void putLE(std::vector<std::byte>& v, T x, std::size_t n) {
  const auto u = static_cast<std::uint64_t>(x);
  for (std::size_t i = 0; i < n; ++i) {
    putU8(v, static_cast<std::uint8_t>((u >> (8 * i)) & 0xFF));
  }
}
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
// A fixed-width NUL-padded field, exactly as DBN writes one.
void putCstr(std::vector<std::byte>& v, const std::string& s, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    putU8(v, i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
  }
}

std::span<const std::byte> asSpan(const std::vector<std::byte>& v) {
  return std::span<const std::byte>(v.data(), v.size());
}

// ===========================================================================
// A minimal DBN writer. Mirrors the encoder in `databento/dbn`
// (`rust/dbn/src/encode/dbn/sync.rs`) for the parts these tests exercise, and
// `DbnEncoderTest.EncoderReproducesEveryFixtureByteForByte` is what proves it
// mirrors it correctly.
// ===========================================================================

struct Interval {
  std::uint32_t startDate;
  std::uint32_t endDate;
  std::string   symbol;
};
struct Mapping {
  std::string           rawSymbol;
  std::vector<Interval> intervals;
};

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

  std::size_t scl() const {
    return version == 1 ? dbn::kSymbolCstrLenV1 : symbolCstrLen;
  }

  void addMbo(const dbn::MboMsg& m) {
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

  void addTrade(const dbn::TradeMsg& m) {
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

  // A header-only record of some other declared rtype, for the
  // "every declared rtype is accepted" test.
  void addOpaque(dbn::RType rtype, std::uint32_t instrumentId, std::uint64_t tsEvent) {
    putU8(records,
          static_cast<std::uint8_t>(dbn::kRecordHeaderLen / dbn::kLengthMultiplier));
    putU8(records, static_cast<std::uint8_t>(rtype));
    putU16(records, 1);
    putU32(records, instrumentId);
    putU64(records, tsEvent);
  }

  std::vector<std::byte> build() const {
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
    EXPECT_EQ(out.size() - fixedStart, dbn::kFixedMetadataLen)
        << "the writer's own fixed metadata block is the wrong size";

    out.insert(out.end(), var.begin(), var.end());
    for (std::size_t i = 0; i < tail; ++i) putU8(out, padByte);
    out.insert(out.end(), records.begin(), records.end());
    return out;
  }
};

// ===========================================================================
// Fixtures
// ===========================================================================

std::vector<std::byte> readFixture(const std::string& name) {
  const std::string path = std::string{GMA_DATA_DBN_FIXTURE_DIR} + "/" + name;
  std::ifstream in(path, std::ios::binary);
  // A MISSING fixture FAILS. It never skips: a skipped fixture test looks
  // exactly like a passing one, and the whole value of this suite is that the
  // bytes are real.
  EXPECT_TRUE(in.good()) << "fixture not readable: " << path;
  std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::vector<std::byte> out(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(raw[i]));
  }
  return out;
}

// The two MboMsg and two TradeMsg the fixtures contain. Cross-checked against
// `databento-cpp`'s DbnDecoderSchemaTests.TestDecodeMbo / TestDecodeTrades;
// also tabulated in fixtures/dbn/PROVENANCE.md.
dbn::MboMsg fixtureMbo(int i) {
  dbn::MboMsg m;
  m.hd.publisherId  = 1;
  m.hd.instrumentId = 5482;
  m.flags           = 128;  // dbn::kFlagLast
  m.channelId       = 0;
  m.action          = static_cast<char>(dbn::Action::Cancel);
  m.side            = static_cast<char>(dbn::Side::Ask);
  m.size            = 1;
  if (i == 0) {
    m.hd.tsEvent = 1609160400000429831ULL;
    m.orderId    = 647784973705ULL;
    m.price      = 3722750000000LL;
    m.tsRecv     = 1609160400000704060ULL;
    m.tsInDelta  = 22993;
    m.sequence   = 1170352;
  } else {
    m.hd.tsEvent = 1609160400000431665ULL;
    m.orderId    = 647784973631ULL;
    m.price      = 3723000000000LL;
    m.tsRecv     = 1609160400000711344ULL;
    m.tsInDelta  = 19621;
    m.sequence   = 1170353;
  }
  return m;
}

dbn::TradeMsg fixtureTrade(int i) {
  dbn::TradeMsg m;
  m.hd.publisherId  = 1;
  m.hd.instrumentId = 5482;
  m.action          = static_cast<char>(dbn::Action::Trade);
  m.side            = static_cast<char>(dbn::Side::Ask);
  m.flags           = 129;  // kFlagLast | kFlagPublisherSpecific
  m.depth           = 0;
  m.price           = 3720250000000LL;
  if (i == 0) {
    m.hd.tsEvent = 1609160400098821953ULL;
    m.size       = 5;
    m.tsRecv     = 1609160400099150057ULL;
    m.tsInDelta  = 19251;
    m.sequence   = 1170380;
  } else {
    m.hd.tsEvent = 1609160400107665963ULL;
    m.size       = 21;
    m.tsRecv     = 1609160400108142648ULL;
    m.tsInDelta  = 20728;
    m.sequence   = 1170414;
  }
  return m;
}

// A writer configured exactly like the committed fixtures, for the byte-exact
// reproduction test.
DbnWriter fixtureWriter(std::uint8_t version, bool mbo) {
  DbnWriter w;
  w.version     = version;
  w.schema      = mbo ? static_cast<std::uint16_t>(dbn::Schema::Mbo)
                      : static_cast<std::uint16_t>(dbn::Schema::Trades);
  w.limit       = 2;
  w.recordCount = 2;  // v1 only; the fixtures carry a real count, not the null
  if (mbo) {
    w.addMbo(fixtureMbo(0));
    w.addMbo(fixtureMbo(1));
  } else {
    w.addTrade(fixtureTrade(0));
    w.addTrade(fixtureTrade(1));
  }
  return w;
}

// A writer with no records and no symbology constraint trouble, for the
// synthetic streams. One instrument, one day-long window covering the
// fixtures' timestamps.
DbnWriter plainWriter() {
  DbnWriter w;
  w.schema = static_cast<std::uint16_t>(dbn::Schema::Mbo);
  return w;
}

constexpr std::uint64_t kTs  = 1609160400000429831ULL;  // inside [20201228, 20201229)
constexpr std::uint32_t kIid = 5482;

dbn::MboMsg mbo(char action, char side, std::uint64_t orderId, std::int64_t price,
                std::uint32_t size, std::uint32_t seq = 1) {
  dbn::MboMsg m;
  m.hd.publisherId  = 1;
  m.hd.instrumentId = kIid;
  m.hd.tsEvent      = kTs;
  m.tsRecv          = kTs + 1000;
  m.orderId         = orderId;
  m.price           = price;
  m.size            = size;
  m.action          = action;
  m.side            = side;
  m.sequence        = seq;
  return m;
}

struct Harness {
  SymbolTable symbols;
  SymbolTable venues;
  std::unique_ptr<DatabentoAdapter> a;

  explicit Harness(DatabentoAdapter::Config cfg = {}) {
    a = std::make_unique<DatabentoAdapter>(std::move(cfg), symbols, venues);
  }
  arrow::Status feed(const std::vector<std::byte>& bytes) { return a->feed(asSpan(bytes)); }
  arrow::Status run(const DbnWriter& w) {
    const auto bytes = w.build();
    ARROW_RETURN_NOT_OK(a->feed(asSpan(bytes)));
    return a->finish();
  }
};

}  // namespace

// ===========================================================================
// The non-circularity gate
// ===========================================================================

TEST(DbnEncoderTest, EncoderReproducesEveryFixtureByteForByte) {
  const struct {
    const char*  file;
    std::uint8_t version;
    bool         mbo;
  } cases[] = {
      {"mbo.v1.dbn", 1, true},   {"mbo.v2.dbn", 2, true},
      {"mbo.v3.dbn", 3, true},   {"trades.v1.dbn", 1, false},
      {"trades.v2.dbn", 2, false},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.file);
    const auto want = readFixture(c.file);
    const auto got  = fixtureWriter(c.version, c.mbo).build();
    ASSERT_EQ(got.size(), want.size())
        << "re-encoding the values we believe " << c.file
        << " holds produced a different LENGTH, so the layout claim is wrong";
    for (std::size_t i = 0; i < want.size(); ++i) {
      ASSERT_EQ(static_cast<unsigned>(std::to_integer<std::uint8_t>(got[i])),
                static_cast<unsigned>(std::to_integer<std::uint8_t>(want[i])))
          << "byte " << i << " differs — the DBN layout in DatabentoAdapter.hpp, or "
             "the values in PROVENANCE.md, are wrong";
    }
  }
}

TEST(DbnEncoderTest, FixturesAreTheBytesProvenanceSays) {
  // Guards against an LFS pointer, a line-ending mangle or a truncated copy
  // sneaking in: the sizes are in PROVENANCE.md and in the vendor repo.
  EXPECT_EQ(readFixture("mbo.v1.dbn").size(), 318u);
  EXPECT_EQ(readFixture("mbo.v2.dbn").size(), 465u);
  EXPECT_EQ(readFixture("mbo.v3.dbn").size(), 472u);
  EXPECT_EQ(readFixture("trades.v1.dbn").size(), 302u);
  EXPECT_EQ(readFixture("trades.v2.dbn").size(), 449u);
}

// ===========================================================================
// Real fixtures -> canonical records
// ===========================================================================

class DbnFixtureTest : public testing::TestWithParam<const char*> {};

INSTANTIATE_TEST_SUITE_P(MboVersions, DbnFixtureTest,
                         testing::Values("mbo.v1.dbn", "mbo.v2.dbn", "mbo.v3.dbn"),
                         [](const testing::TestParamInfo<const char*>& i) {
                           return std::string{"v"} + i.param[5];  // "mbo.vN.dbn"
                         });

TEST_P(DbnFixtureTest, MboDecodesToTheVendorsValues) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture(GetParam())).ok());
  ASSERT_TRUE(h.a->finish().ok());

  ASSERT_TRUE(h.a->metadata().has_value());
  const auto& md = *h.a->metadata();
  EXPECT_EQ(md.dataset, "GLBX.MDP3");
  ASSERT_TRUE(md.schema.has_value());
  EXPECT_EQ(*md.schema, static_cast<std::uint16_t>(dbn::Schema::Mbo));
  EXPECT_EQ(md.start, 1609160400000000000ULL);
  EXPECT_EQ(md.end, 1609200000000000000ULL);
  EXPECT_EQ(md.limit, 2u);
  ASSERT_TRUE(md.stypeIn.has_value());
  EXPECT_EQ(*md.stypeIn, static_cast<std::uint8_t>(dbn::SType::RawSymbol));
  EXPECT_EQ(md.stypeOut, static_cast<std::uint8_t>(dbn::SType::InstrumentId));
  EXPECT_FALSE(md.tsOut);
  ASSERT_EQ(md.symbols.size(), 1u);
  EXPECT_EQ(md.symbols[0], "ESH1");
  EXPECT_TRUE(md.partial.empty());
  EXPECT_TRUE(md.notFound.empty());
  ASSERT_EQ(md.mappings.size(), 1u);
  EXPECT_EQ(md.mappings[0].rawSymbol, "ESH1");
  ASSERT_EQ(md.mappings[0].intervals.size(), 1u);
  EXPECT_EQ(md.mappings[0].intervals[0].startDate, 20201228u);
  EXPECT_EQ(md.mappings[0].intervals[0].endDate, 20201229u);
  EXPECT_EQ(md.mappings[0].intervals[0].symbol, "5482");

  EXPECT_EQ(h.a->stats().recordsIn, 2u);
  EXPECT_TRUE(h.a->trades().empty()) << "an mbo file produces no `trades` rows";
  ASSERT_EQ(h.a->bookEvents().size(), 2u);

  // Both fixture records are Cancel/Ask for an order whose Add we never saw,
  // so both resolve to a Delete with the removed quantity and both count as an
  // unknown order. That is the documented untracked-cancel path.
  EXPECT_EQ(h.a->stats().unknownOrder, 2u);

  const std::uint32_t ticker = h.symbols.lookup("ESH1");
  ASSERT_NE(ticker, kInvalidSymbolId);
  const std::uint32_t venue = h.venues.lookup("GLBX.MDP3");
  ASSERT_NE(venue, kInvalidSymbolId);

  for (int i = 0; i < 2; ++i) {
    SCOPED_TRACE(i);
    const dbn::MboMsg want = fixtureMbo(i);
    const BookRecord& got  = h.a->bookEvents()[static_cast<std::size_t>(i)];
    EXPECT_EQ(got.header.tsEventNs, static_cast<std::int64_t>(want.hd.tsEvent));
    EXPECT_EQ(got.header.tsRecvNs, static_cast<std::int64_t>(want.tsRecv));
    EXPECT_EQ(got.header.providerSeq, want.sequence);
    EXPECT_EQ(got.header.instrumentId, 5482u);
    EXPECT_EQ(got.header.symbolId, ticker);
    EXPECT_EQ(got.header.venueId, venue);
    EXPECT_EQ(got.action, BookAction::Delete);
    EXPECT_EQ(got.side, Side::Ask);
    EXPECT_EQ(got.price, want.price);
    EXPECT_EQ(got.size, want.size);
    EXPECT_EQ(got.orderId, want.orderId);
    EXPECT_EQ(got.priority, want.sequence);
    EXPECT_EQ(got.depth, 0u);
    EXPECT_EQ(got.epoch, 0u);
    EXPECT_EQ(got.flags, want.flags);
  }
}

TEST_P(DbnFixtureTest, MetadataVersionDifferencesAreTheOnesMeasured) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture(GetParam())).ok());
  const auto& md = *h.a->metadata();
  if (md.version == 1) {
    EXPECT_EQ(md.symbolCstrLen, dbn::kSymbolCstrLenV1);
    ASSERT_TRUE(md.recordCount.has_value()) << "v1 carries a record_count";
    EXPECT_EQ(*md.recordCount, 2u);
    EXPECT_EQ(md.endPadding, 0u);
  } else if (md.version == 2) {
    EXPECT_EQ(md.symbolCstrLen, 71u);
    EXPECT_FALSE(md.recordCount.has_value());
    EXPECT_EQ(md.endPadding, 0u) << "only v3 aligns its metadata";
  } else {
    EXPECT_EQ(md.version, 3u);
    EXPECT_EQ(md.symbolCstrLen, 71u);
    EXPECT_FALSE(md.recordCount.has_value());
    // The measured v3 difference: 353 bytes of metadata rounded up to 360.
    EXPECT_EQ(md.endPadding, 7u);
  }
}

TEST_P(DbnFixtureTest, EveryRowCarriesTsEventProviderSeqAndInstrumentId) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture(GetParam())).ok());
  ASSERT_FALSE(h.a->bookEvents().empty());
  for (const BookRecord& r : h.a->bookEvents()) {
    EXPECT_GT(r.header.tsEventNs, 0) << "ts_event is the only time an operator may key off";
    EXPECT_GT(r.header.providerSeq, 0u) << "provider_seq comes from DBN and is never null";
    EXPECT_NE(r.header.instrumentId, 0u) << "instrument_id is retained for provenance";
    EXPECT_NE(r.header.symbolId, kInvalidSymbolId);
  }
}

TEST_P(DbnFixtureTest, FeedingOneByteAtATimeProducesIdenticalRecords) {
  // NOTE FOR A FUTURE CLEANUP: this is NOT the test that covers buffer
  // compaction. One byte at a time means `drain()` always consumes everything
  // or nothing, so the `cursor_ > 0 && cursor_ < size` partial-compaction
  // branch never runs here. `APartialRecordAtTheEndOfTheStreamIsAGap` is the
  // only test that exercises it (and
  // `CorruptedOrTruncatedBytesRefuseButNeverCrash` hits it incidentally).
  // Do not delete either as redundant with this one.
  const auto bytes = readFixture(GetParam());

  Harness whole;
  ASSERT_TRUE(whole.feed(bytes).ok());
  ASSERT_TRUE(whole.a->finish().ok());

  Harness drip;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    ASSERT_TRUE(drip.a->feed(std::span<const std::byte>(bytes.data() + i, 1)).ok())
        << "byte " << i;
    // Mid-record, nothing may be emitted and the tail must be held.
    EXPECT_LE(drip.a->bookEvents().size(), whole.a->bookEvents().size());
  }
  ASSERT_TRUE(drip.a->finish().ok());

  ASSERT_EQ(drip.a->bookEvents().size(), whole.a->bookEvents().size());
  for (std::size_t i = 0; i < whole.a->bookEvents().size(); ++i) {
    const auto& x = whole.a->bookEvents()[i];
    const auto& y = drip.a->bookEvents()[i];
    EXPECT_EQ(x.header.tsEventNs, y.header.tsEventNs);
    EXPECT_EQ(x.header.providerSeq, y.header.providerSeq);
    EXPECT_EQ(x.price, y.price);
    EXPECT_EQ(x.size, y.size);
    EXPECT_EQ(x.orderId, y.orderId);
    EXPECT_EQ(x.action, y.action);
  }
  EXPECT_EQ(drip.a->pendingBytes(), 0u);
}

class DbnTradesFixtureTest : public testing::TestWithParam<const char*> {};

INSTANTIATE_TEST_SUITE_P(TradesVersions, DbnTradesFixtureTest,
                         testing::Values("trades.v1.dbn", "trades.v2.dbn"),
                         [](const testing::TestParamInfo<const char*>& i) {
                           return std::string{"v"} + i.param[8];  // "trades.vN.dbn"
                         });

TEST_P(DbnTradesFixtureTest, TradesDecodeToTheVendorsValues) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture(GetParam())).ok());
  ASSERT_TRUE(h.a->finish().ok());

  ASSERT_TRUE(h.a->metadata().has_value());
  ASSERT_TRUE(h.a->metadata()->schema.has_value());
  EXPECT_EQ(*h.a->metadata()->schema, static_cast<std::uint16_t>(dbn::Schema::Trades));

  EXPECT_TRUE(h.a->bookEvents().empty()) << "a trades file produces no book_events rows";
  ASSERT_EQ(h.a->trades().size(), 2u);

  for (int i = 0; i < 2; ++i) {
    SCOPED_TRACE(i);
    const dbn::TradeMsg want = fixtureTrade(i);
    const TickRecord& got    = h.a->trades()[static_cast<std::size_t>(i)];
    EXPECT_EQ(got.kind, RecordKind::Trade);
    EXPECT_EQ(got.header.tsEventNs, static_cast<std::int64_t>(want.hd.tsEvent));
    EXPECT_EQ(got.header.tsRecvNs, static_cast<std::int64_t>(want.tsRecv));
    EXPECT_EQ(got.header.providerSeq, want.sequence);
    EXPECT_EQ(got.header.instrumentId, 5482u);
    EXPECT_EQ(got.price, want.price);
    EXPECT_EQ(got.size, want.size);
    // DBN side 'A' on a trade is a SELL aggressor.
    EXPECT_EQ(got.aggressor, Aggressor::Sell);
    EXPECT_TRUE(got.bookNeutral) << "SPEC D12/C13: a `trades` print must never consume "
                                    "the book, because the same execution is also in "
                                    "book_events";
    EXPECT_EQ(got.flags, want.flags);
    EXPECT_EQ(got.tradeId, 0u) << "DBN TradeMsg carries no trade id";
  }
}

// ===========================================================================
// D8 — prices are int64 at 1e-9 with no double round trip
// ===========================================================================

TEST(DbnPriceTest, FixturePricesAreExactIntegersAtTheGlobalScale) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 2u);
  // 3722.75 and 3723.00 exactly — quarter-tick ES prices. The assertion is on
  // the INTEGER, deliberately: the point of D8 is that no double is involved,
  // so comparing a double here would test the opposite of the invariant.
  EXPECT_EQ(h.a->bookEvents()[0].price, 3722750000000LL);
  EXPECT_EQ(h.a->bookEvents()[1].price, 3723000000000LL);
  EXPECT_EQ(h.a->bookEvents()[0].price % (kPriceScale / 1000), 0LL);
  // A round trip through double would lose nothing at THIS magnitude, which is
  // exactly why the test asserts the integer path instead of a tolerance.
  EXPECT_EQ(kPriceScale, 1'000'000'000LL);
}

TEST(DbnPriceTest, UndefPriceBecomesKNoPriceAndNeverZero) {
  DbnWriter w = plainWriter();
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), static_cast<char>(dbn::Side::Bid), 1,
               dbn::kUndefPrice, 7));
  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 1u);
  EXPECT_EQ(h.a->bookEvents()[0].price, kNoPrice);
  EXPECT_NE(h.a->bookEvents()[0].price, 0LL) << "0 is a legal price";
  EXPECT_EQ(h.a->stats().undefPrice, 1u);
}

// ===========================================================================
// The action mapping, against ItchAdapter, for EVERY declared action
// ===========================================================================

TEST(DbnActionMappingTest, EveryDeclaredActionIsAcceptedAndMapsLikeItchAdapter) {
  // The mapping table, enumerated — not spot-checked. Each row names the ITCH
  // message ItchAdapter handles, and the BookRecords this adapter must produce.
  //
  //  DBN | ITCH analogue                  | rows emitted
  //  ----+--------------------------------+-------------------------------
  //   A  | add_order / add_order_mpid     | [Add(size)]
  //   M  | order_replace                  | [Update(size)]  (same order_id)
  //   C  | order_cancel / order_delete    | [Update(rem)] or [Delete(removed)]
  //   F  | order_executed                 | [Trade]          (book-neutral)
  //   T  | trade (top-level print)        | [Trade]          (book-neutral)
  //   R  | (no ITCH analogue)             | [Clear] + epoch bump
  //   N  | (no ITCH analogue)             | []               (accepted, no row)
  struct Row {
    dbn::Action                action;
    std::vector<BookAction>    expect;
    const char*                itch;
  };
  const std::vector<Row> table = {
      {dbn::Action::Add,    {BookAction::Add},    "add_order / add_order_mpid"},
      {dbn::Action::Modify, {BookAction::Update}, "order_replace"},
      {dbn::Action::Cancel, {BookAction::Delete}, "order_cancel / order_delete"},
      {dbn::Action::Fill,   {BookAction::Trade},  "order_executed"},
      {dbn::Action::Trade,  {BookAction::Trade},  "trade"},
      {dbn::Action::Clear,  {BookAction::Clear},  "(none)"},
      {dbn::Action::None,   {},                   "(none)"},
  };

  // Every declared enumerator appears in the table exactly once. This is what
  // stops the table drifting out of date against dbn::Action.
  ASSERT_EQ(table.size(), dbn::kAllActions.size());
  for (const dbn::Action a : dbn::kAllActions) {
    int seen = 0;
    for (const auto& r : table) seen += (r.action == a) ? 1 : 0;
    EXPECT_EQ(seen, 1) << "action " << dbn::actionName(static_cast<char>(a))
                       << " is declared but not in the mapping table";
  }

  for (const Row& row : table) {
    const char ch = static_cast<char>(row.action);
    SCOPED_TRACE(std::string{"action "} + dbn::actionName(ch));
    ASSERT_TRUE(dbn::isDeclaredAction(ch)) << "declared enumerator must be accepted";

    DbnWriter w = plainWriter();
    // Cancel resolves against a tracked order, so for the Cancel row the order
    // is NOT pre-added: a cancel of an order we never saw is a full Delete,
    // which is the row the table asserts. The partial/full split is tested
    // separately below.
    w.addMbo(mbo(ch, static_cast<char>(dbn::Side::Bid), 42, 1'000'000'000LL, 3));

    Harness h;
    const arrow::Status st = h.run(w);
    ASSERT_TRUE(st.ok()) << st.ToString();
    ASSERT_EQ(h.a->bookEvents().size(), row.expect.size())
        << "wrong number of book rows for ITCH analogue " << row.itch;
    for (std::size_t i = 0; i < row.expect.size(); ++i) {
      EXPECT_EQ(h.a->bookEvents()[i].action, row.expect[i]);
    }
    EXPECT_TRUE(h.a->trades().empty()) << "mbo records never produce `trades` rows";
  }
}

TEST(DbnActionMappingTest, EveryDeclaredSideIsAccepted) {
  for (const dbn::Side s : dbn::kAllSides) {
    const char ch = static_cast<char>(s);
    SCOPED_TRACE(std::string{"side "} + dbn::sideName(ch));
    ASSERT_TRUE(dbn::isDeclaredSide(ch));

    DbnWriter w = plainWriter();
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add), ch, 1, 1'000'000'000LL, 1));
    Harness h;
    const arrow::Status st = h.run(w);
    ASSERT_TRUE(st.ok()) << st.ToString();
    ASSERT_EQ(h.a->bookEvents().size(), 1u);
    const BookRecord& r = h.a->bookEvents()[0];
    switch (s) {
      case dbn::Side::Bid:
        EXPECT_EQ(r.side, Side::Bid);
        EXPECT_EQ(r.flags & kGmaFlagSideUnspecified, 0);
        break;
      case dbn::Side::Ask:
        EXPECT_EQ(r.side, Side::Ask);
        EXPECT_EQ(r.flags & kGmaFlagSideUnspecified, 0);
        break;
      case dbn::Side::None:
        // data::Side has no third value, so the gap is flagged rather than
        // flattened onto Bid without a trace.
        EXPECT_NE(r.flags & kGmaFlagSideUnspecified, 0);
        EXPECT_EQ(h.a->stats().sideUnspecified, 1u);
        break;
    }
  }
}

TEST(DbnActionMappingTest, EveryDeclaredRTypeIsAccepted) {
  for (const dbn::RType t : dbn::kAllRTypes) {
    SCOPED_TRACE(static_cast<int>(t));
    ASSERT_TRUE(dbn::isDeclaredRType(static_cast<std::uint8_t>(t)));

    DbnWriter w = plainWriter();
    const bool consumed = t == dbn::RType::Mbo || t == dbn::RType::Mbp0;
    if (t == dbn::RType::Mbo) {
      w.addMbo(mbo(static_cast<char>(dbn::Action::Add),
                   static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1));
    } else if (t == dbn::RType::Mbp0) {
      dbn::TradeMsg m;
      m.hd.publisherId  = 1;
      m.hd.instrumentId = kIid;
      m.hd.tsEvent      = kTs;
      m.tsRecv          = kTs;
      m.price           = 1'000'000'000LL;
      m.size            = 2;
      m.action          = static_cast<char>(dbn::Action::Trade);
      m.side            = static_cast<char>(dbn::Side::Bid);
      m.sequence        = 9;
      w.addTrade(m);
    } else {
      w.addOpaque(t, kIid, kTs);
    }

    Harness h;
    const arrow::Status st = h.run(w);
    ASSERT_TRUE(st.ok()) << "declared rtype refused: " << st.ToString();
    EXPECT_EQ(h.a->stats().recordsIn, 1u);
    // A declared rtype outside the first slice's two schemas is SKIPPED and
    // COUNTED, never silently dropped and never a refusal.
    EXPECT_EQ(h.a->stats().recordsSkipped, consumed ? 0u : 1u);
  }
}

TEST(DbnActionMappingTest, AnUndeclaredActionSideOrRTypeIsRefused) {
  {
    DbnWriter w = plainWriter();
    w.addMbo(mbo('X', static_cast<char>(dbn::Side::Bid), 1, 1, 1));
    Harness h;
    const arrow::Status st = h.run(w);
    EXPECT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("action"), std::string::npos) << st.ToString();
  }
  {
    DbnWriter w = plainWriter();
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add), 'Z', 1, 1, 1));
    Harness h;
    const arrow::Status st = h.run(w);
    EXPECT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("side"), std::string::npos) << st.ToString();
  }
  {
    // 0x7F is not a declared DBN rtype.
    DbnWriter w = plainWriter();
    w.addOpaque(static_cast<dbn::RType>(0x7F), kIid, kTs);
    Harness h;
    const arrow::Status st = h.run(w);
    EXPECT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("rtype"), std::string::npos) << st.ToString();
  }
  EXPECT_FALSE(dbn::isDeclaredAction('X'));
  EXPECT_FALSE(dbn::isDeclaredSide('Z'));
  EXPECT_FALSE(dbn::isDeclaredRType(0x7F));
}

TEST(DbnActionMappingTest, CancelIsADeltaResolvedAgainstTheRestingSize) {
  // ItchAdapter::routeOrderCancel: a cancel of fewer shares than remain is an
  // UPDATE to the remainder; a cancel of at least as many is a DELETE.
  DbnWriter w = plainWriter();
  const char add = static_cast<char>(dbn::Action::Add);
  const char can = static_cast<char>(dbn::Action::Cancel);
  const char bid = static_cast<char>(dbn::Side::Bid);
  w.addMbo(mbo(add, bid, 100, 1'000'000'000LL, 10, 1));
  w.addMbo(mbo(can, bid, 100, 1'000'000'000LL, 3, 2));   // partial: 10 -> 7
  w.addMbo(mbo(can, bid, 100, 1'000'000'000LL, 7, 3));   // exact: empties it
  w.addMbo(mbo(add, bid, 200, 2'000'000'000LL, 5, 4));
  w.addMbo(mbo(can, bid, 200, 2'000'000'000LL, 99, 5));  // over-cancel: delete

  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 5u);

  EXPECT_EQ(h.a->bookEvents()[0].action, BookAction::Add);
  EXPECT_EQ(h.a->bookEvents()[0].size, 10u);

  EXPECT_EQ(h.a->bookEvents()[1].action, BookAction::Update);
  EXPECT_EQ(h.a->bookEvents()[1].size, 7u)
      << "an Update's size is the order's ABSOLUTE remaining size, not the delta";

  EXPECT_EQ(h.a->bookEvents()[2].action, BookAction::Delete);
  EXPECT_EQ(h.a->bookEvents()[2].size, 7u)
      << "a Delete's size is the quantity REMOVED — a deleted order has no remainder";

  EXPECT_EQ(h.a->bookEvents()[4].action, BookAction::Delete);
  EXPECT_EQ(h.a->bookEvents()[4].size, 99u);

  // The tracked order is gone after each delete, and never went negative.
  EXPECT_FALSE(h.a->restingSize(kIid, 100).has_value());
  EXPECT_FALSE(h.a->restingSize(kIid, 200).has_value());
  // Neither delete was an "unknown order" — both Adds were seen.
  EXPECT_EQ(h.a->stats().unknownOrder, 0u);
}

TEST(DbnActionMappingTest, ACancelForAnUnseenOrderIsAFullDeleteAndIsCounted) {
  DbnWriter w = plainWriter();
  w.addMbo(mbo(static_cast<char>(dbn::Action::Cancel),
               static_cast<char>(dbn::Side::Ask), 999, 1'000'000'000LL, 4));
  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 1u);
  EXPECT_EQ(h.a->bookEvents()[0].action, BookAction::Delete);
  EXPECT_EQ(h.a->bookEvents()[0].size, 4u);
  EXPECT_EQ(h.a->stats().unknownOrder, 1u);
}

TEST(DbnActionMappingTest, ModifyIsOneUpdateKeepingTheOrderId) {
  // ITCH's order_replace issues a NEW order ref, so ItchAdapter emits
  // delete+add. DBN's Modify keeps the same order_id and carries the new
  // absolute price and size, so it is one Update — a wire difference, not a
  // semantic one. The resting size must follow the modify, or the next Cancel
  // resolves against a stale number.
  DbnWriter w = plainWriter();
  const char bid = static_cast<char>(dbn::Side::Bid);
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), bid, 7, 1'000'000'000LL, 10, 1));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Modify), bid, 7, 1'500'000'000LL, 4, 2));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Cancel), bid, 7, 1'500'000'000LL, 1, 3));

  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 3u);
  EXPECT_EQ(h.a->bookEvents()[1].action, BookAction::Update);
  EXPECT_EQ(h.a->bookEvents()[1].orderId, 7u);
  EXPECT_EQ(h.a->bookEvents()[1].price, 1'500'000'000LL);
  EXPECT_EQ(h.a->bookEvents()[1].size, 4u);
  EXPECT_EQ(h.a->bookEvents()[2].action, BookAction::Update);
  EXPECT_EQ(h.a->bookEvents()[2].size, 3u) << "4 - 1, i.e. the modify's size was adopted";
  EXPECT_EQ(h.a->stats().unknownOrder, 0u);
}

TEST(DbnActionMappingTest, ATradeOrFillNeverConsumesTheBook) {
  // The invariant the ticket calls load-bearing. DBN's own words for both
  // actions are "Does not affect the book", and every ObTradeEvent in this
  // repo is bookNeutral. So a Trade/Fill must produce EXACTLY ONE row, with
  // action Trade, and must not touch the tracked resting size.
  DbnWriter w = plainWriter();
  const char bid = static_cast<char>(dbn::Side::Bid);
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), bid, 5, 1'000'000'000LL, 10, 1));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Fill), bid, 5, 1'000'000'000LL, 4, 2));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Trade), bid, 0, 1'000'000'000LL, 4, 3));

  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 3u);
  EXPECT_EQ(h.a->bookEvents()[1].action, BookAction::Trade);
  EXPECT_EQ(h.a->bookEvents()[2].action, BookAction::Trade);
  ASSERT_TRUE(h.a->restingSize(kIid, 5).has_value());
  EXPECT_EQ(*h.a->restingSize(kIid, 5), 10u)
      << "the Fill print must not have consumed the resting order";
}

TEST(DbnActionMappingTest, FillMutatesTheBookOnlyWhenConfiguredTo) {
  // The measurement hook. With fillReducesRestingOrder the adapter takes
  // ItchAdapter's branch: the book mutation FIRST, then the print.
  DbnWriter w = plainWriter();
  const char bid = static_cast<char>(dbn::Side::Bid);
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), bid, 5, 1'000'000'000LL, 10, 1));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Fill), bid, 5, 1'000'000'000LL, 4, 2));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Fill), bid, 5, 1'000'000'000LL, 6, 3));

  DatabentoAdapter::Config cfg;
  cfg.fillReducesRestingOrder = true;
  Harness h(cfg);
  ASSERT_TRUE(h.run(w).ok());
  // Add, then (Update + Trade), then (Delete + Trade).
  ASSERT_EQ(h.a->bookEvents().size(), 5u);
  EXPECT_EQ(h.a->bookEvents()[0].action, BookAction::Add);
  EXPECT_EQ(h.a->bookEvents()[1].action, BookAction::Update);
  EXPECT_EQ(h.a->bookEvents()[1].size, 6u);
  EXPECT_EQ(h.a->bookEvents()[2].action, BookAction::Trade);
  EXPECT_EQ(h.a->bookEvents()[3].action, BookAction::Delete);
  EXPECT_EQ(h.a->bookEvents()[4].action, BookAction::Trade);
  EXPECT_FALSE(h.a->restingSize(kIid, 5).has_value());
}

TEST(DbnActionMappingTest, ATradeSideIsTheAggressorAndAFillSideIsFlipped) {
  // DBN: side is "the side of the market for RESTING orders, or the side of
  // the AGGRESSOR for trades". A Fill names the resting order, so the
  // aggressor is the opposite — ItchAdapter::routeOrderExecuted's
  // "The resting order's side is passive; the aggressor is the opposite".
  // Not flipping would put two different meanings in one column.
  DbnWriter w = plainWriter();
  w.addMbo(mbo(static_cast<char>(dbn::Action::Trade),
               static_cast<char>(dbn::Side::Bid), 0, 1'000'000'000LL, 1, 1));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Fill),
               static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1, 2));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Fill),
               static_cast<char>(dbn::Side::Ask), 2, 1'000'000'000LL, 1, 3));

  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 3u);
  EXPECT_EQ(h.a->bookEvents()[0].side, Side::Bid) << "a Trade already names the aggressor";
  EXPECT_EQ(h.a->bookEvents()[1].side, Side::Ask) << "a Fill on a resting BID is a SELL";
  EXPECT_EQ(h.a->bookEvents()[2].side, Side::Bid) << "a Fill on a resting ASK is a BUY";
}

TEST(DbnActionMappingTest, EveryBookMutationRowCarriesTheRestingSide) {
  // The column `BookRecord::side` holds TWO meanings — the resting order's side
  // on Add/Update/Delete/Clear, the aggressor's on a Trade row (see the header).
  // A column with two meanings needs every row asserted, or the meanings swap
  // silently. `ItchAdapter` is structurally incapable of this bug: its mutation
  // events (`ObDeleteEvent{symbol, orderRef}`,
  // `ObUpdateEvent{symbol, orderRef, nullopt, remaining}`) carry no side at all
  // and only `ObTradeEvent` gets the flipped aggressor. The side on a mutation
  // row is a column THIS adapter added, so it is this adapter's job to prove it.
  //
  // The case that was wrong: with `fillReducesRestingOrder` a Fill emits BOTH a
  // mutation row and a print, and the aggressor flip leaked onto the mutation.
  const char bid = static_cast<char>(dbn::Side::Bid);
  const char ask = static_cast<char>(dbn::Side::Ask);

  for (const char restingSide : {bid, ask}) {
    SCOPED_TRACE(dbn::sideName(restingSide));
    const Side wantResting  = restingSide == bid ? Side::Bid : Side::Ask;
    const Side wantAggressor = restingSide == bid ? Side::Ask : Side::Bid;

    DbnWriter w = plainWriter();
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add), restingSide, 1,
                 1'000'000'000LL, 10, 1));
    w.addMbo(mbo(static_cast<char>(dbn::Action::Cancel), restingSide, 1,
                 1'000'000'000LL, 3, 2));   // partial -> Update
    w.addMbo(mbo(static_cast<char>(dbn::Action::Modify), restingSide, 1,
                 1'100'000'000LL, 5, 3));   // -> Update
    w.addMbo(mbo(static_cast<char>(dbn::Action::Fill), restingSide, 1,
                 1'100'000'000LL, 2, 4));   // -> Update(resting) + Trade(aggressor)
    w.addMbo(mbo(static_cast<char>(dbn::Action::Fill), restingSide, 1,
                 1'100'000'000LL, 9, 5));   // -> Delete(resting) + Trade(aggressor)
    w.addMbo(mbo(static_cast<char>(dbn::Action::Clear), restingSide, 0,
                 dbn::kUndefPrice, 0, 6));  // -> Clear(resting)

    DatabentoAdapter::Config cfg;
    cfg.fillReducesRestingOrder = true;
    Harness h(cfg);
    ASSERT_TRUE(h.run(w).ok());
    ASSERT_EQ(h.a->bookEvents().size(), 8u);

    for (std::size_t i = 0; i < h.a->bookEvents().size(); ++i) {
      const BookRecord& r = h.a->bookEvents()[i];
      SCOPED_TRACE(static_cast<int>(r.action));
      if (r.action == BookAction::Trade) {
        EXPECT_EQ(r.side, wantAggressor)
            << "row " << i << ": a Trade row carries the AGGRESSOR side";
      } else {
        EXPECT_EQ(r.side, wantResting)
            << "row " << i << ": action " << static_cast<int>(r.action)
            << " is a book MUTATION and must carry the RESTING side, not the "
               "aggressor the Fill print needs";
      }
    }
  }
}

TEST(DbnActionMappingTest, ActionNoneIsAcceptedAndProducesNoBookRow) {
  DbnWriter w = plainWriter();
  w.addMbo(mbo(static_cast<char>(dbn::Action::None),
               static_cast<char>(dbn::Side::None), 0, dbn::kUndefPrice, 0));
  Harness h;
  ASSERT_TRUE(h.run(w).ok()) << "Action::None is declared, so it must be ACCEPTED";
  EXPECT_TRUE(h.a->bookEvents().empty())
      << "there is no BookAction for 'no effect on the book'; inventing one would put "
         "a no-op mutation into the replay stream";
  EXPECT_EQ(h.a->stats().actionNone, 1u);
  EXPECT_EQ(h.a->stats().recordsIn, 1u);
}

TEST(DbnActionMappingTest, ClearBumpsTheEpochAndDropsTrackedOrders) {
  DbnWriter w = plainWriter();
  const char bid = static_cast<char>(dbn::Side::Bid);
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), bid, 11, 1'000'000'000LL, 9, 1));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Clear), bid, 0, dbn::kUndefPrice, 0, 2));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), bid, 12, 1'000'000'000LL, 8, 3));
  w.addMbo(mbo(static_cast<char>(dbn::Action::Cancel), bid, 11, 1'000'000'000LL, 9, 4));

  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 4u);
  EXPECT_EQ(h.a->bookEvents()[0].epoch, 0u);
  EXPECT_EQ(h.a->bookEvents()[1].action, BookAction::Clear);
  EXPECT_EQ(h.a->bookEvents()[1].epoch, 1u)
      << "the Clear row carries the NEW epoch, matching OrderBookManager::onReset";
  EXPECT_EQ(h.a->bookEvents()[2].epoch, 1u) << "every row after a Clear carries it too";
  EXPECT_EQ(h.a->epochOf(kIid), 1u);
  EXPECT_EQ(h.a->stats().clears, 1u);
  // The pre-Clear order is no longer tracked, so its post-Clear cancel cannot
  // be resolved against stale state. THE MECHANISM IS THE ERASE LOOP in the
  // Clear case, not an epoch in the adapter's key: this adapter's private
  // `OrderKey` is `{instrumentId, orderId}` and carries no epoch. (The epoch'd
  // key is `gma::OrderKey`, which protects the BOOK downstream.) Removing the
  // erase loop is what this assertion catches.
  EXPECT_EQ(h.a->stats().unknownOrder, 1u);
  EXPECT_EQ(h.a->trackedOrders(), 1u) << "only the post-Clear order 12 survives";
  EXPECT_FALSE(h.a->restingSize(kIid, 11).has_value());
  EXPECT_EQ(h.a->bookEvents()[3].epoch, 1u);
}

TEST(DbnActionMappingTest, TheOrderMapIsBounded) {
  // ASSERT THE INVARIANT, NOT THE COUNTER. An earlier version of this test
  // checked only `evictedOrders > 0`, which a no-op eviction that still bumps
  // the counter satisfies — so replacing `orders_.erase(begin())` with nothing
  // left the suite green while the map grew without limit. The bound itself is
  // `trackedOrders() <= maxTrackedOrders`, and it has to hold after EVERY
  // record, not just at the end.
  constexpr std::size_t kCap = 4;
  constexpr std::uint32_t kAdds = 20;

  DatabentoAdapter::Config cfg;
  cfg.maxTrackedOrders = kCap;
  Harness h(cfg);

  // Fed one record at a time so the bound is checked at every step; a
  // whole-stream feed could only ever see the final state.
  for (std::uint32_t i = 0; i < kAdds; ++i) {
    DbnWriter w = plainWriter();
    if (i > 0) w.records.clear();
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add), static_cast<char>(dbn::Side::Bid),
                 1000 + i, 1'000'000'000LL, 1, i + 1));
    const auto bytes = w.build();
    if (i == 0) {
      ASSERT_TRUE(h.feed(bytes).ok());
    } else {
      // Metadata is already consumed; feed just this record's bytes.
      const std::vector<std::byte> rec(bytes.end() - static_cast<std::ptrdiff_t>(
                                                         dbn::kMboMsgLen),
                                       bytes.end());
      ASSERT_TRUE(h.a->feed(asSpan(rec)).ok()) << "record " << i;
    }
    EXPECT_LE(h.a->trackedOrders(), kCap)
        << "after record " << i << " the tracked-order map exceeded its cap";
  }
  ASSERT_TRUE(h.a->finish().ok());
  EXPECT_EQ(h.a->bookEvents().size(), kAdds);
  EXPECT_EQ(h.a->trackedOrders(), kCap);
  EXPECT_EQ(h.a->stats().evictedOrders, kAdds - kCap);

  // At most `kCap` of the 20 order ids may still resolve, and the ones that do
  // must report the size they were added with — eviction drops tracking, it
  // does not corrupt it.
  std::size_t stillTracked = 0;
  for (std::uint32_t i = 0; i < kAdds; ++i) {
    if (const auto sz = h.a->restingSize(kIid, 1000 + i)) {
      ++stillTracked;
      EXPECT_EQ(*sz, 1u);
    }
  }
  EXPECT_EQ(stillTracked, kCap);
}

TEST(DbnGatewayTest, ChannelIdAndTsInDeltaAreDecodedAtTheRightOffsets) {
  // `ts_in_delta` is the only signed-32 field in either DBN record and it has
  // no canonical column, so without `gateway()` nothing could gate that load
  // at all — zeroing both `loadI32` call sites used to leave the suite green.
  // `channel_id` matters for a second reason: it is the scope DBN's `sequence`
  // (our `provider_seq`) is monotonic within, so a stream mixing channels is
  // one where `(session_id, provider_seq)` is not a per-symbol total order
  // (ENC-1483/T6).
  {
    Harness h;
    ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
    // The second (last) fixture record's values.
    EXPECT_EQ(h.a->gateway().tsInDelta, 19621);
    EXPECT_EQ(h.a->gateway().channelId, 0u);
    EXPECT_EQ(h.a->gateway().minChannelId, 0u);
    EXPECT_EQ(h.a->gateway().maxChannelId, 0u)
        << "the fixture is a single channel, so sequence is a total order in it";
  }
  {
    // TradeMsg carries ts_in_delta at a DIFFERENT offset from MboMsg's, so the
    // trades fixture gates the second load site.
    Harness h;
    ASSERT_TRUE(h.feed(readFixture("trades.v2.dbn")).ok());
    EXPECT_EQ(h.a->gateway().tsInDelta, 20728);
  }
  {
    // A NEGATIVE ts_in_delta, which is what makes the field signed and is the
    // one value an unsigned load would get wrong, plus two channels so
    // min != max actually reports a mixed stream.
    DbnWriter w = plainWriter();
    dbn::MboMsg a = mbo(static_cast<char>(dbn::Action::Add),
                        static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1, 1);
    a.channelId = 3;
    a.tsInDelta = -12345;
    dbn::MboMsg b = a;
    b.orderId    = 2;
    b.sequence   = 2;
    b.channelId  = 9;
    b.tsInDelta  = 7;
    w.addMbo(a);
    w.addMbo(b);
    Harness h;
    ASSERT_TRUE(h.run(w).ok());
    EXPECT_EQ(h.a->gateway().tsInDelta, 7);
    EXPECT_EQ(h.a->gateway().channelId, 9u);
    EXPECT_EQ(h.a->gateway().minChannelId, 3u);
    EXPECT_EQ(h.a->gateway().maxChannelId, 9u)
        << "min != max is the two-byte answer to 'did this stream mix channels?'";
  }
  {
    // And the sign survives on its own, as the LAST record seen.
    DbnWriter w = plainWriter();
    dbn::MboMsg a = mbo(static_cast<char>(dbn::Action::Add),
                        static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1, 1);
    a.tsInDelta = -2'000'000'000;  // near INT32_MIN
    w.addMbo(a);
    Harness h;
    ASSERT_TRUE(h.run(w).ok());
    EXPECT_EQ(h.a->gateway().tsInDelta, -2'000'000'000);
  }
}

// ===========================================================================
// Framing refusals
// ===========================================================================

TEST(DbnFramingTest, APartialRecordAtTheEndOfTheStreamIsAGap) {
  // LOAD-BEARING BEYOND ITS NAME: by feeding a whole truncated stream in one
  // call it is the only test that leaves `drain()` with a partial record AND
  // consumed bytes behind it, i.e. the only one that exercises the
  // `cursor_ > 0` compaction branch. A one-byte-at-a-time feed cannot.
  auto bytes = readFixture("mbo.v2.dbn");
  bytes.resize(bytes.size() - 9);  // chop the tail of the second record
  Harness h;
  ASSERT_TRUE(h.feed(bytes).ok()) << "a partial record is held, not refused, mid-stream";
  EXPECT_EQ(h.a->bookEvents().size(), 1u);
  EXPECT_EQ(h.a->pendingBytes(), dbn::kMboMsgLen - 9);
  const arrow::Status st = h.a->finish();
  EXPECT_TRUE(st.IsInvalid()) << "end-of-stream mid-record is a GAP";
  EXPECT_NE(st.message().find("gap"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, AStreamThatEndsInsideItsMetadataIsRefused) {
  auto bytes = readFixture("mbo.v2.dbn");
  bytes.resize(100);
  Harness h;
  ASSERT_TRUE(h.feed(bytes).ok());
  EXPECT_FALSE(h.a->metadata().has_value());
  EXPECT_TRUE(h.a->finish().IsInvalid());
}

TEST(DbnFramingTest, AZstdFrameIsNamedRatherThanMisparsed) {
  std::vector<std::byte> z;
  for (const std::uint8_t b : dbn::kZstdMagic) putU8(z, b);
  putZeros(z, 32);
  Harness h;
  const arrow::Status st = h.feed(z);
  ASSERT_TRUE(st.IsInvalid());
  EXPECT_NE(st.message().find("zstd"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, NonDbnMagicIsRefused) {
  std::vector<std::byte> junk;
  for (const char c : std::string{"NOTDBN!!"}) {
    putU8(junk, static_cast<std::uint8_t>(c));
  }
  Harness h;
  const arrow::Status st = h.feed(junk);
  ASSERT_TRUE(st.IsInvalid());
  EXPECT_NE(st.message().find("DBN"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, AnUnknownDbnVersionIsRefused) {
  for (const std::uint8_t v : {std::uint8_t{0}, std::uint8_t{4}, std::uint8_t{250}}) {
    SCOPED_TRACE(static_cast<int>(v));
    auto bytes  = readFixture("mbo.v2.dbn");
    bytes[3]    = std::byte{v};
    Harness h;
    const arrow::Status st = h.feed(bytes);
    ASSERT_TRUE(st.IsInvalid()) << "version " << static_cast<int>(v) << " must be refused";
    EXPECT_NE(st.message().find("version"), std::string::npos) << st.ToString();
  }
}

TEST(DbnFramingTest, ARefusalPoisonsTheAdapter) {
  DbnWriter w = plainWriter();
  w.addMbo(mbo('X', static_cast<char>(dbn::Side::Bid), 1, 1, 1));
  Harness h;
  const arrow::Status first = h.feed(w.build());
  ASSERT_TRUE(first.IsInvalid());
  // A later, perfectly good feed must NOT resume: the decoder has proven it
  // cannot parse this stream, and resynchronising would invent a boundary.
  const arrow::Status second = h.feed(readFixture("mbo.v2.dbn"));
  EXPECT_TRUE(second.IsInvalid());
  EXPECT_EQ(second.message(), first.message());
  EXPECT_TRUE(h.a->finish().IsInvalid());
}

TEST(DbnFramingTest, TsOutIsRefusedRatherThanMisread) {
  DbnWriter w = plainWriter();
  w.tsOut = 1;
  Harness h;
  const arrow::Status st = h.feed(w.build());
  ASSERT_TRUE(st.IsInvalid());
  EXPECT_NE(st.message().find("ts_out"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, AWrongRecordLengthIsRefused) {
  // Claim an MboMsg is 48 bytes rather than 56.
  auto bytes = readFixture("mbo.v2.dbn");
  const std::size_t firstRecord = bytes.size() - 2 * dbn::kMboMsgLen;
  bytes[firstRecord] = std::byte{static_cast<std::uint8_t>(dbn::kTradeMsgLen /
                                                           dbn::kLengthMultiplier)};
  Harness h;
  const arrow::Status st = h.feed(bytes);
  ASSERT_TRUE(st.IsInvalid()) << st.ToString();
  EXPECT_NE(st.message().find("MboMsg"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, ARecordShorterThanItsHeaderIsRefused) {
  DbnWriter w = plainWriter();
  putU8(w.records, 2);  // 2 * 4 = 8 bytes, i.e. shorter than the 16-byte header
  putZeros(w.records, 7);
  Harness h;
  EXPECT_TRUE(h.feed(w.build()).IsInvalid());
}

TEST(DbnFramingTest, AMetadataLengthShorterThanTheFixedBlockIsRefused) {
  DbnWriter w = plainWriter();
  w.forceMetadataLen = 32;
  Harness h;
  const arrow::Status st = h.feed(w.build());
  ASSERT_TRUE(st.IsInvalid());
  EXPECT_NE(st.message().find("metadata length"), std::string::npos) << st.ToString();
}

TEST(DbnFramingTest, AMetadataLengthThatCutsTheVariableSectionIsRefused) {
  DbnWriter w = plainWriter();
  // Long enough to pass the minimum check, short enough to truncate the
  // symbol/mapping section the real metadata needs.
  w.forceMetadataLen = static_cast<std::uint32_t>(dbn::kFixedMetadataLen + 24);
  Harness h;
  const arrow::Status st = h.feed(w.build());
  EXPECT_TRUE(st.IsInvalid()) << st.ToString();
}

TEST(DbnFramingTest, TheMetadataTailIsOnlyEverV3AlignmentPadding) {
  // The guard this exercises was the ONE thing the mutation sweep for this
  // ticket found untested: flipping `if (version < kFirstAlignedVersion || ...)`
  // to `if (false)` broke nothing. These four cases are why it is a guard.

  // (a) The natural v3 tail — 353 bytes of metadata rounded up to 360 — is
  //     ACCEPTED, and reported rather than swallowed.
  {
    DbnWriter w = plainWriter();
    w.version = 3;
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add),
                 static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1));
    Harness h;
    ASSERT_TRUE(h.run(w).ok());
    ASSERT_TRUE(h.a->metadata().has_value());
    EXPECT_EQ(h.a->metadata()->endPadding, 7u);
  }
  // (b) v1 and v2 pad nothing, so ANY tail there is a layout we do not
  //     understand rather than padding we may skip.
  for (const std::uint8_t version : {std::uint8_t{1}, std::uint8_t{2}}) {
    SCOPED_TRACE(static_cast<int>(version));
    DbnWriter w   = plainWriter();
    w.version     = version;
    w.recordCount = 1;
    w.extraTail   = 3;
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add),
                 static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1));
    Harness h;
    const arrow::Status st = h.feed(w.build());
    ASSERT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("trailing byte"), std::string::npos) << st.ToString();
  }
  // (c) A v3 tail of 8 or more bytes is not alignment padding either — the
  //     encoder pads by 1..7 and never by a whole word.
  {
    DbnWriter w = plainWriter();
    w.version   = 3;
    w.extraTail = 1;  // natural 7 + 1 == 8
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add),
                 static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1));
    Harness h;
    const arrow::Status st = h.feed(w.build());
    ASSERT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("trailing byte"), std::string::npos) << st.ToString();
  }
  // (d) v3 padding that is not zero is refused: a non-zero tail is data in a
  //     field we are about to skip, and skipping data is how a layout change
  //     becomes a silent misparse.
  {
    DbnWriter w = plainWriter();
    w.version   = 3;
    w.padByte   = 0xFF;
    w.addMbo(mbo(static_cast<char>(dbn::Action::Add),
                 static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1));
    Harness h;
    const arrow::Status st = h.feed(w.build());
    ASSERT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("not zero"), std::string::npos) << st.ToString();
  }
}

TEST(DbnFramingTest, CorruptedOrTruncatedBytesRefuseButNeverCrash) {
  // LOAD-BEARING BEYOND ITS NAME: this is the only test that catches a
  // disabled `repeated` count bound in `parseMetadata` (the guard that stops a
  // corrupt symbol count from reserving gigabytes). It is also what exercises
  // ASan/UBSan. Do not weaken it to a handful of cases.
  // A hand-rolled byte parser's characteristic failure is an out-of-bounds read
  // on input it was never shown, and `Reader`/`drain()` are full of
  // length-derived offsets. So drive every single-byte corruption and every
  // truncation of a real fixture through it and require only that the outcome
  // is a VERDICT: Ok or Invalid, never a crash, never a hang, and never a
  // record emitted from a stream that was refused.
  //
  // Deterministic by construction — every mutation is enumerated, none random —
  // so this cannot flake. Run the suite under ASan/UBSan and this test is what
  // exercises them: 1,860 corruptions plus 466 truncations of mbo.v2.dbn.
  const auto base = readFixture("mbo.v2.dbn");
  ASSERT_FALSE(base.empty());

  std::size_t ok = 0;
  std::size_t refused = 0;
  for (std::size_t i = 0; i < base.size(); ++i) {
    for (const std::uint8_t mask : {std::uint8_t{0x00}, std::uint8_t{0xFF},
                                    std::uint8_t{0x55}, std::uint8_t{0x01}}) {
      auto bytes = base;
      bytes[i] = std::byte{static_cast<std::uint8_t>(
          mask == 0x55 ? (std::to_integer<std::uint8_t>(base[i]) ^ 0x55) : mask)};
      Harness h;
      const arrow::Status st = h.a->feed(asSpan(bytes));
      if (st.ok()) {
        ++ok;
        // Whatever it accepted must still be internally consistent.
        for (const BookRecord& r : h.a->bookEvents()) {
          EXPECT_NE(r.header.symbolId, kInvalidSymbolId);
          EXPECT_GT(r.header.tsEventNs, 0);
        }
      } else {
        ++refused;
        EXPECT_TRUE(st.IsInvalid()) << "byte " << i << ": " << st.ToString();
        // A refused feed appends nothing beyond what earlier whole records in
        // the same feed had already produced, and the adapter stays poisoned.
        EXPECT_TRUE(h.a->feed(asSpan(base)).IsInvalid());
      }
    }
  }
  for (std::size_t n = 0; n <= base.size(); ++n) {
    const std::vector<std::byte> bytes(base.begin(),
                                       base.begin() + static_cast<std::ptrdiff_t>(n));
    Harness h;
    const arrow::Status fed = h.a->feed(asSpan(bytes));
    EXPECT_TRUE(fed.ok() || fed.IsInvalid()) << "truncation " << n;
    const bool heldBytes = h.a->pendingBytes() != 0;
    const bool haveMeta   = h.a->metadata().has_value();
    const arrow::Status fin = h.a->finish();
    // `finish()` is clean EXACTLY when the stream ended on a boundary: metadata
    // read, and no bytes held mid-record. That includes a metadata-only file
    // (n == 353) and a file cut after its first whole record (n == 409) — a DBN
    // file carrying zero or one records is complete, not truncated, and
    // reporting a gap there would make every short legitimate pull an error.
    // Anything else is a GAP.
    EXPECT_EQ(fin.ok(), haveMeta && !heldBytes)
        << "truncation " << n << ": finish()=" << fin.ToString()
        << " pending=" << h.a->pendingBytes() << " meta=" << haveMeta;
    if (!fin.ok()) {
      EXPECT_TRUE(fin.IsInvalid()) << fin.ToString();
    }
  }
  // Both finish() outcomes must occur across the sweep, or the EXPECT_EQ above
  // is comparing two constants.
  {
    Harness onBoundary;
    ASSERT_TRUE(onBoundary.a->feed(asSpan(base)).ok());
    EXPECT_TRUE(onBoundary.a->finish().ok());
    Harness midRecord;
    const std::vector<std::byte> cut(base.begin(), base.end() - 9);
    ASSERT_TRUE(midRecord.a->feed(asSpan(cut)).ok());
    EXPECT_TRUE(midRecord.a->finish().IsInvalid());
  }
  // Both outcomes must actually occur, or this test is asserting nothing: a
  // corruption that is always refused would mean the loop never reached the
  // accept path, and one that is never refused would mean the guards are dead.
  EXPECT_GT(refused, 0u);
  EXPECT_GT(ok, 0u);
}

// ===========================================================================
// Symbology (SPEC D7)
// ===========================================================================

TEST(DbnSymbologyTest, InstrumentIdResolvesThroughTheMappingWindow) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
  ASSERT_FALSE(h.a->bookEvents().empty());
  const SymbolId id = h.a->bookEvents()[0].header.symbolId;
  EXPECT_EQ(h.symbols.name(id), "ESH1")
      << "`symbol` is the TICKER, not the instrument_id (SPEC D7)";
  EXPECT_EQ(h.a->bookEvents()[0].header.instrumentId, 5482u)
      << "instrument_id is retained for provenance";
}

TEST(DbnSymbologyTest, ARecordOutsideEveryWindowIsRefused) {
  DbnWriter w = plainWriter();
  dbn::MboMsg m = mbo(static_cast<char>(dbn::Action::Add),
                      static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1);
  // One day past the mapping's half-open end (20201229).
  m.hd.tsEvent = 1609160400000429831ULL + 2 * 86'400'000'000'000ULL;
  w.addMbo(m);
  Harness h;
  const arrow::Status st = h.feed(w.build());
  ASSERT_TRUE(st.IsInvalid()) << st.ToString();
  EXPECT_NE(st.message().find("symbology"), std::string::npos) << st.ToString();
}

TEST(DbnSymbologyTest, TheWindowIsHalfOpen) {
  DbnWriter w = plainWriter();
  w.mappings = {Mapping{"ESH1", {Interval{20201228, 20201229, "5482"}}}};
  // 2020-12-29T00:00:00Z exactly — the excluded end of [20201228, 20201229).
  dbn::MboMsg m = mbo(static_cast<char>(dbn::Action::Add),
                      static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1);
  m.hd.tsEvent = 1609200000000000000ULL;
  w.addMbo(m);
  Harness h;
  EXPECT_TRUE(h.feed(w.build()).IsInvalid())
      << "[start, end) — a closed upper bound is what double-delivers a boundary row";
}

TEST(DbnSymbologyTest, AnUnmappedInstrumentIdIsRefused) {
  DbnWriter w = plainWriter();
  dbn::MboMsg m = mbo(static_cast<char>(dbn::Action::Add),
                      static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1);
  m.hd.instrumentId = 7777;  // the metadata names 5482 only
  w.addMbo(m);
  Harness h;
  const arrow::Status st = h.feed(w.build());
  ASSERT_TRUE(st.IsInvalid()) << st.ToString();
  EXPECT_NE(st.message().find("instrument_id"), std::string::npos) << st.ToString();
}

TEST(DbnSymbologyTest, MetadataWithNoInstrumentIdMappingIsRefusedUnlessOverridden) {
  DbnWriter w = plainWriter();
  // Neither stype mentions InstrumentId, so nothing maps an id to a ticker.
  w.stypeIn  = static_cast<std::uint8_t>(dbn::SType::RawSymbol);
  w.stypeOut = static_cast<std::uint8_t>(dbn::SType::RawSymbol);
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), static_cast<char>(dbn::Side::Bid), 1,
               1'000'000'000LL, 1));
  {
    Harness h;
    const arrow::Status st = h.feed(w.build());
    ASSERT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("symbolOverride"), std::string::npos) << st.ToString();
  }
  {
    DatabentoAdapter::Config cfg;
    cfg.symbolOverride = "ESH1";
    Harness h(cfg);
    ASSERT_TRUE(h.run(w).ok());
    ASSERT_EQ(h.a->bookEvents().size(), 1u);
    EXPECT_EQ(h.symbols.name(h.a->bookEvents()[0].header.symbolId), "ESH1");
  }
}

TEST(DbnSymbologyTest, AReverseDirectionMappingResolvesToo) {
  // stype_in == InstrumentId: rawSymbol is the numeric id and the interval
  // carries the ticker. Both directions are handled rather than one assumed.
  DbnWriter w = plainWriter();
  w.stypeIn  = static_cast<std::uint8_t>(dbn::SType::InstrumentId);
  w.stypeOut = static_cast<std::uint8_t>(dbn::SType::RawSymbol);
  w.symbols  = {"5482"};
  w.mappings = {Mapping{"5482", {Interval{20201228, 20201229, "ESH1"}}}};
  w.addMbo(mbo(static_cast<char>(dbn::Action::Add), static_cast<char>(dbn::Side::Bid), 1,
               1'000'000'000LL, 1));
  Harness h;
  ASSERT_TRUE(h.run(w).ok());
  ASSERT_EQ(h.a->bookEvents().size(), 1u);
  EXPECT_EQ(h.symbols.name(h.a->bookEvents()[0].header.symbolId), "ESH1");
}

TEST(DbnSymbologyTest, VenueDefaultsToTheDatasetAndConfigWins) {
  {
    Harness h;
    ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
    EXPECT_EQ(h.venues.name(h.a->bookEvents()[0].header.venueId), "GLBX.MDP3");
  }
  {
    DatabentoAdapter::Config cfg;
    cfg.venue = "XNAS";
    Harness h(cfg);
    ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
    EXPECT_EQ(h.venues.name(h.a->bookEvents()[0].header.venueId), "XNAS");
  }
}

TEST(DbnSymbologyTest, PublisherIdChangingMidStreamIsRefused) {
  DbnWriter w = plainWriter();
  dbn::MboMsg a = mbo(static_cast<char>(dbn::Action::Add),
                      static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1, 1);
  dbn::MboMsg b = a;
  b.hd.publisherId = 2;
  b.sequence       = 2;
  w.addMbo(a);
  w.addMbo(b);
  Harness h;
  const arrow::Status st = h.feed(w.build());
  ASSERT_TRUE(st.IsInvalid()) << st.ToString();
  EXPECT_NE(st.message().find("publisher_id"), std::string::npos) << st.ToString();
}

// ===========================================================================
// Sentinels and the header
// ===========================================================================

TEST(DbnHeaderTest, AnUndefinedTsEventIsRefusedAndAnUndefinedTsRecvIsZero) {
  {
    DbnWriter w = plainWriter();
    dbn::MboMsg m = mbo(static_cast<char>(dbn::Action::Add),
                        static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1);
    m.hd.tsEvent = dbn::kUndefTimestamp;
    w.addMbo(m);
    Harness h;
    const arrow::Status st = h.feed(w.build());
    ASSERT_TRUE(st.IsInvalid()) << st.ToString();
    EXPECT_NE(st.message().find("ts_event"), std::string::npos) << st.ToString();
  }
  {
    DbnWriter w = plainWriter();
    dbn::MboMsg m = mbo(static_cast<char>(dbn::Action::Add),
                        static_cast<char>(dbn::Side::Bid), 1, 1'000'000'000LL, 1);
    m.tsRecv = dbn::kUndefTimestamp;
    w.addMbo(m);
    Harness h;
    ASSERT_TRUE(h.run(w).ok()) << "ts_recv is diagnostics only; it must not fail a row";
    ASSERT_EQ(h.a->bookEvents().size(), 1u);
    EXPECT_EQ(h.a->bookEvents()[0].header.tsRecvNs, 0);
  }
}

TEST(DbnHeaderTest, SessionIdIsStampedOnEveryRow) {
  DatabentoAdapter::Config cfg;
  cfg.sessionId = 77;
  Harness h(cfg);
  ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
  ASSERT_FALSE(h.a->bookEvents().empty());
  for (const BookRecord& r : h.a->bookEvents()) EXPECT_EQ(r.header.sessionId, 77u);
}

// ===========================================================================
// The deliverable: canonical Arrow batches
// ===========================================================================

TEST(DbnBatchTest, FixturesBecomeCanonicalBatchesThatRoundTrip) {
  Harness mboH;
  ASSERT_TRUE(mboH.feed(readFixture("mbo.v2.dbn")).ok());
  ASSERT_TRUE(mboH.a->finish().ok());

  const auto bookBatch = mboH.a->bookEventsBatch();
  ASSERT_TRUE(bookBatch.ok()) << bookBatch.status().ToString();
  ASSERT_EQ((*bookBatch)->num_rows(), 2);
  EXPECT_TRUE((*bookBatch)->schema()->Equals(*bookEventsSchema()));

  std::vector<BookRecord> back;
  ASSERT_TRUE(decodeBookEvents(**bookBatch, mboH.symbols, mboH.venues, back).ok());
  ASSERT_EQ(back.size(), mboH.a->bookEvents().size());
  for (std::size_t i = 0; i < back.size(); ++i) {
    EXPECT_EQ(back[i].header.tsEventNs, mboH.a->bookEvents()[i].header.tsEventNs);
    EXPECT_EQ(back[i].header.providerSeq, mboH.a->bookEvents()[i].header.providerSeq);
    EXPECT_EQ(back[i].header.instrumentId, mboH.a->bookEvents()[i].header.instrumentId);
    EXPECT_EQ(back[i].price, mboH.a->bookEvents()[i].price);
    EXPECT_EQ(back[i].size, mboH.a->bookEvents()[i].size);
    EXPECT_EQ(back[i].orderId, mboH.a->bookEvents()[i].orderId);
    EXPECT_EQ(back[i].action, mboH.a->bookEvents()[i].action);
    EXPECT_EQ(back[i].side, mboH.a->bookEvents()[i].side);
    EXPECT_EQ(back[i].flags, mboH.a->bookEvents()[i].flags);
  }

  Harness trH;
  ASSERT_TRUE(trH.feed(readFixture("trades.v2.dbn")).ok());
  ASSERT_TRUE(trH.a->finish().ok());
  const auto tradeBatch = trH.a->tradesBatch();
  ASSERT_TRUE(tradeBatch.ok()) << tradeBatch.status().ToString();
  ASSERT_EQ((*tradeBatch)->num_rows(), 2);
  EXPECT_TRUE((*tradeBatch)->schema()->Equals(*tradesSchema()));

  std::vector<TickRecord> ticks;
  ASSERT_TRUE(
      decodeTicks(**tradeBatch, RecordKind::Trade, trH.symbols, trH.venues, ticks).ok());
  ASSERT_EQ(ticks.size(), 2u);
  for (std::size_t i = 0; i < ticks.size(); ++i) {
    EXPECT_EQ(ticks[i].price, trH.a->trades()[i].price);
    EXPECT_EQ(ticks[i].size, trH.a->trades()[i].size);
    EXPECT_EQ(ticks[i].aggressor, trH.a->trades()[i].aggressor);
    EXPECT_TRUE(ticks[i].bookNeutral);
  }
}

TEST(DbnBatchTest, TakeDrainsAndTheBatchIsEmptyAfterwards) {
  Harness h;
  ASSERT_TRUE(h.feed(readFixture("mbo.v2.dbn")).ok());
  const auto taken = h.a->takeBookEvents();
  EXPECT_EQ(taken.size(), 2u);
  EXPECT_TRUE(h.a->bookEvents().empty());
  const auto batch = h.a->bookEventsBatch();
  ASSERT_TRUE(batch.ok());
  EXPECT_EQ((*batch)->num_rows(), 0);
}

}  // namespace gma::data
