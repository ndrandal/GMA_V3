// Canonical L1 schemas and the POD <-> Arrow codec (ENC-1481).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D3, D7, D8;
//         docs/data-layer.md §5.3, §8.3, §8.4.
//
// The acceptance criterion of ENC-1481 is "a unit test round-trips each POD
// through its Arrow schema", and the round trip is asserted FIELD BY FIELD
// rather than with a memcmp or an operator==. The reason is that a field the
// codec silently never writes is exactly what a whole-struct comparison of
// two default-constructed-then-partially-filled records can hide: both sides
// carry the default and the comparison is true. So every fixture value below
// is DISTINCT FROM THE FIELD'S DEFAULT, and `AllFixtureFieldsDifferFromDefault`
// pins that property so the round-trip tests cannot go vacuous later.

#include "gma/data/Record.hpp"
#include "gma/data/Schema.hpp"
#include "gma/data/SymbolTable.hpp"

#include <arrow/array/array_base.h>
#include <arrow/array/array_dict.h>
#include <arrow/array/array_binary.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/ipc/options.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/io/memory.h>
#include <arrow/util/compression.h>
#include <arrow/util/key_value_metadata.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace gma::data;

// --- fixtures --------------------------------------------------------------

struct Tables {
  SymbolTable symbols;
  SymbolTable venues;
  SymbolId aapl = symbols.intern("AAPL");
  SymbolId msft = symbols.intern("MSFT");
  SymbolId xnas = venues.intern("XNAS");
  SymbolId otc  = venues.intern("OTCM");
};

RecordHeader headerA(const Tables& t) {
  RecordHeader h;
  h.tsEventNs = 1'728'900'000'123'456'789LL;
  h.tsRecvNs  = 1'728'900'000'123'499'999LL;
  h.providerSeq = 42'000'000'001ULL;
  h.sessionId = 7;
  h.symbolId = t.aapl;
  h.venueId = t.xnas;
  h.instrumentId = 32'145;
  return h;
}

RecordHeader headerB(const Tables& t) {
  RecordHeader h;
  h.tsEventNs = -1'000'000'000LL;   // pre-epoch: a negative nanosecond is legal
  h.tsRecvNs  = 0;
  h.providerSeq = 1;
  h.sessionId = 4'294'967'295U;     // UINT32_MAX
  h.symbolId = t.msft;
  h.venueId = t.otc;
  h.instrumentId = 0;
  return h;
}

void expectHeaderEq(const RecordHeader& got, const RecordHeader& want) {
  EXPECT_EQ(got.tsEventNs, want.tsEventNs);
  EXPECT_EQ(got.tsRecvNs, want.tsRecvNs);
  EXPECT_EQ(got.providerSeq, want.providerSeq);
  EXPECT_EQ(got.sessionId, want.sessionId);
  EXPECT_EQ(got.symbolId, want.symbolId);
  EXPECT_EQ(got.venueId, want.venueId);
  EXPECT_EQ(got.instrumentId, want.instrumentId);
}

std::vector<TickRecord> tradeFixtures(const Tables& t) {
  TickRecord a;
  a.header = headerA(t);
  a.kind = RecordKind::Trade;
  a.price = 190'125'000'000LL;   // $190.125 at the global 1e-9 scale
  a.size = 1'500;
  a.aggressor = Aggressor::Sell;
  a.bookNeutral = true;          // the ObTradeEvent::bookNeutral precedent
  a.tradeId = 18'446'744'073'709'551'615ULL;  // UINT64_MAX
  a.flags = 0xBEEF;

  TickRecord b;
  b.header = headerB(t);
  b.kind = RecordKind::Trade;
  b.price = -2'500'000'000LL;    // negative prices are legal (bonds/spreads)
  b.size = 1;
  b.aggressor = Aggressor::Buy;
  b.bookNeutral = false;
  b.tradeId = 0;
  b.flags = 1;

  // A third row re-using the FIRST symbol, so the dictionary column has fewer
  // entries than rows. Without this the dictionary is the identity and the
  // index mapping is never exercised.
  TickRecord c = a;
  c.header.providerSeq = 42'000'000'002ULL;
  c.price = 190'130'000'000LL;
  c.flags = 0;

  return {a, b, c};
}

std::vector<TickRecord> quoteFixtures(const Tables& t) {
  TickRecord a;
  a.header = headerA(t);
  a.kind = RecordKind::Quote;
  a.bidPx = 190'120'000'000LL;
  a.askPx = 190'130'000'000LL;
  a.bidSz = 300;
  a.askSz = 400;
  a.bidCt = 5;
  a.askCt = 6;
  a.flags = 0x00FF;

  TickRecord b;
  b.header = headerB(t);
  b.kind = RecordKind::Quote;
  b.bidPx = kNoPrice;   // an empty side must survive as kNoPrice, not as 0
  b.askPx = 1;
  b.bidSz = 0;
  b.askSz = 18'446'744'073'709'551'615ULL;
  b.bidCt = 0;
  b.askCt = 4'294'967'295U;
  b.flags = 0;

  return {a, b};
}

std::vector<BookRecord> bookFixtures(const Tables& t) {
  BookRecord a;
  a.header = headerA(t);
  a.action = BookAction::Add;
  a.side = Side::Bid;
  a.price = 190'120'000'000LL;
  a.size = 100;
  a.orderId = 9'876'543'210ULL;
  a.priority = 1'234'567ULL;
  a.depth = 3;
  a.epoch = 11;
  a.flags = 0x0102;

  BookRecord b;
  b.header = headerB(t);
  b.action = BookAction::Clear;   // the epoch-bumping action
  b.side = Side::Ask;
  b.price = kNoPrice;
  b.size = 0;
  b.orderId = 0;                  // OrderKey::idMissing
  b.priority = 18'446'744'073'709'551'615ULL;
  b.depth = 65'535;
  b.epoch = 12;
  b.flags = 0;

  BookRecord c;
  c.header = headerA(t);
  c.header.providerSeq = 42'000'000'003ULL;
  c.action = BookAction::TickSize;
  c.side = Side::Bid;
  c.price = 10'000'000LL;
  c.size = 0;
  c.orderId = 0;
  c.priority = 0;
  c.depth = 0;
  c.epoch = 11;
  c.flags = 0;

  return {a, b, c};
}

std::vector<SnapshotRecord> snapshotFixtures(const Tables& t) {
  SnapshotRecord a;
  a.header = headerA(t);
  a.snapshotSeq = 777'000'111ULL;
  a.level.side = Side::Bid;
  a.level.price = 190'120'000'000LL;
  a.level.size = 2'500;
  a.level.orderCount = 9;

  SnapshotRecord b;
  b.header = headerA(t);
  b.snapshotSeq = 777'000'111ULL;  // same snapshot, second level
  b.level.side = Side::Ask;
  b.level.price = 190'130'000'000LL;
  b.level.size = 1'800;
  b.level.orderCount = 4;

  SnapshotRecord c;
  c.header = headerB(t);
  c.snapshotSeq = 0;
  c.level.side = Side::Ask;
  c.level.price = kNoPrice;
  c.level.size = 0;
  c.level.orderCount = 0;

  return {a, b, c};
}

// --- batch surgery ---------------------------------------------------------
//
// The checks below need a batch whose COLUMN TYPES are canonical and whose
// CONTENT is not: an out-of-domain enum byte, a null in a non-nullable column,
// a num_rows that lies. None of those is reachable through the encoder — that
// is the point, they model a batch arriving over ENC-1485's `FORMAT
// ArrowStream` from something this codec does not control — so they are built
// by replacing one column of a legally encoded batch.

std::shared_ptr<arrow::RecordBatch> withColumnReplaced(
    const arrow::RecordBatch& batch, const char* name,
    const std::shared_ptr<arrow::Array>& replacement) {
  auto cols = batch.columns();
  const int idx = batch.schema()->GetFieldIndex(name);
  // GetFieldIndex returns -1 for a name that is not there, and
  // cols[static_cast<size_t>(-1)] is an out-of-bounds WRITE -- in a helper
  // whose whole subject is refusing out-of-bounds indexing. Return the batch
  // unmodified and let the caller's assertion be the thing that fails.
  if (idx < 0) {
    ADD_FAILURE() << "withColumnReplaced: no column named '" << name
                  << "' in " << batch.schema()->ToString();
    return arrow::RecordBatch::Make(batch.schema(), batch.num_rows(), cols);
  }
  cols[static_cast<std::size_t>(idx)] = replacement;
  return arrow::RecordBatch::Make(batch.schema(), batch.num_rows(), cols);
}

std::shared_ptr<arrow::Array> uint8Column(std::initializer_list<int> values) {
  arrow::UInt8Builder b;
  for (const int v : values) {
    EXPECT_TRUE(b.Append(static_cast<std::uint8_t>(v)).ok());
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return out;
}

std::shared_ptr<arrow::Array> int64Column(std::initializer_list<std::int64_t> values) {
  arrow::Int64Builder b;
  for (const std::int64_t v : values) {
    EXPECT_TRUE(b.Append(v).ok());
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return out;
}

// An int64 column with a null in the middle. `price` is declared non-nullable,
// so this is not a batch the PODs have a representation for — and an unchecked
// decode reads Value(i) on the null slot and gets a plausible zero.
std::shared_ptr<arrow::Array> int64ColumnWithANull() {
  arrow::Int64Builder b;
  EXPECT_TRUE(b.Append(1).ok());
  EXPECT_TRUE(b.AppendNull().ok());
  EXPECT_TRUE(b.Append(3).ok());
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Schema shape
// ---------------------------------------------------------------------------

TEST(SchemaTest, EveryTableCarriesTheCommonColumnsFirstAndNonNullable) {
  // docs/data-layer.md §8.3 — same seven columns, same order, on every table.
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"ts_event", "timestamp[ns, tz=UTC]"},
      {"ts_recv", "timestamp[ns, tz=UTC]"},
      {"provider_seq", "uint64"},
      {"session_id", "uint32"},
      {"symbol", "dictionary<values=string, indices=int32, ordered=0>"},
      {"instrument_id", "uint32"},
      {"venue", "dictionary<values=string, indices=int16, ordered=0>"},
  };

  for (const auto& schema : {tradesSchema(), quotesSchema(), bookEventsSchema(),
                             bookSnapshotsSchema()}) {
    ASSERT_NE(schema, nullptr);
    ASSERT_GE(schema->num_fields(), static_cast<int>(expected.size()));
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto& f = *schema->field(static_cast<int>(i));
      EXPECT_EQ(f.name(), expected[i].first);
      EXPECT_EQ(f.type()->ToString(), expected[i].second) << "column " << f.name();
    }
    // Every column, common and payload, is non-nullable: a nullable column is
    // a branch on every read, and nothing in L1 is optional by design.
    for (int i = 0; i < schema->num_fields(); ++i) {
      EXPECT_FALSE(schema->field(i)->nullable())
          << schema->field(i)->name() << " must be non-nullable";
    }
    EXPECT_EQ(schemaVersionOf(*schema), kSchemaVersion);
  }
}

TEST(SchemaTest, SchemaForCoversEveryKindAndSchemasAreStable) {
  EXPECT_TRUE(schemaFor(RecordKind::Trade)->Equals(*tradesSchema()));
  EXPECT_TRUE(schemaFor(RecordKind::Quote)->Equals(*quotesSchema()));
  EXPECT_TRUE(schemaFor(RecordKind::BookEvent)->Equals(*bookEventsSchema()));
  EXPECT_TRUE(schemaFor(RecordKind::BookSnapshot)->Equals(*bookSnapshotsSchema()));

  // The accessors cache in a function-local static. --gtest_repeat re-runs
  // every test body in the SAME process (ENC-1065/ENC-1102), so the cache is
  // process-global state these tests touch; pin that it is immutable and
  // identity-stable rather than discovering it on iteration 2.
  EXPECT_EQ(tradesSchema().get(), tradesSchema().get());
  EXPECT_EQ(bookEventsSchema().get(), bookEventsSchema().get());
  EXPECT_EQ(bookSnapshotsSchema().get(), bookSnapshotsSchema().get());
  EXPECT_EQ(quotesSchema().get(), quotesSchema().get());
}

TEST(SchemaTest, TheThreeTablesAreDistinct) {
  // A union table was explicitly rejected (docs/data-layer.md §8.4); if two of
  // these ever compared equal, requireCanonicalBatch() could not tell them
  // apart and decoding a book batch as trades would read `action` as a price.
  EXPECT_FALSE(tradesSchema()->Equals(*bookEventsSchema()));
  EXPECT_FALSE(tradesSchema()->Equals(*quotesSchema()));
  EXPECT_FALSE(bookEventsSchema()->Equals(*bookSnapshotsSchema()));
}

TEST(SchemaTest, SchemaVersionOfSaysSoWhenItDoesNotKnow) {
  auto bare = arrow::schema({arrow::field("x", arrow::int64())});
  EXPECT_EQ(schemaVersionOf(*bare), -1);
  auto junk = arrow::schema({arrow::field("x", arrow::int64())},
                            arrow::key_value_metadata(
                                {std::string(kSchemaVersionKey)}, {"not-a-number"}));
  EXPECT_EQ(schemaVersionOf(*junk), -1);
}

// ---------------------------------------------------------------------------
// Anti-vacuity for the round trips below
// ---------------------------------------------------------------------------

TEST(RoundTripTest, AllFixtureFieldsDifferFromDefault) {
  // If a fixture field equalled its default, the matching EXPECT in the round
  // trip would pass even if the codec never touched that column. This test is
  // what stops that happening silently as fields are added.
  Tables t;
  const TickRecord trade = tradeFixtures(t).front();
  const TickRecord dflt;
  EXPECT_NE(trade.price, dflt.price);
  EXPECT_NE(trade.size, dflt.size);
  EXPECT_NE(trade.aggressor, dflt.aggressor);
  EXPECT_NE(trade.bookNeutral, dflt.bookNeutral);
  EXPECT_NE(trade.tradeId, dflt.tradeId);
  EXPECT_NE(trade.flags, dflt.flags);

  const TickRecord quote = quoteFixtures(t).front();
  EXPECT_NE(quote.bidPx, dflt.bidPx);
  EXPECT_NE(quote.askPx, dflt.askPx);
  EXPECT_NE(quote.bidSz, dflt.bidSz);
  EXPECT_NE(quote.askSz, dflt.askSz);
  EXPECT_NE(quote.bidCt, dflt.bidCt);
  EXPECT_NE(quote.askCt, dflt.askCt);

  const auto books = bookFixtures(t);
  const BookRecord book = books.front();
  const BookRecord bdflt;
  // `action` and `side` default to Add/Bid, which fixture a legitimately
  // carries, so the non-default assertion has to come from the SET: at least
  // one fixture must exercise a non-default value for each, or the column
  // could be hardcoded and every row would still compare equal.
  EXPECT_TRUE(std::any_of(books.begin(), books.end(), [&](const BookRecord& r) {
    return r.action != bdflt.action;
  })) << "no fixture exercises a non-default BookAction";
  EXPECT_TRUE(std::any_of(books.begin(), books.end(), [&](const BookRecord& r) {
    return r.side != bdflt.side;
  })) << "no fixture exercises a non-default Side";
  EXPECT_NE(book.price, bdflt.price);
  EXPECT_NE(book.size, bdflt.size);
  EXPECT_NE(book.orderId, bdflt.orderId);
  EXPECT_NE(book.priority, bdflt.priority);
  EXPECT_NE(book.depth, bdflt.depth);
  EXPECT_NE(book.epoch, bdflt.epoch);
  EXPECT_NE(book.flags, bdflt.flags);

  const SnapshotRecord snap = snapshotFixtures(t).front();
  const SnapshotRecord sdflt;
  EXPECT_NE(snap.snapshotSeq, sdflt.snapshotSeq);
  EXPECT_NE(snap.level.price, sdflt.level.price);
  EXPECT_NE(snap.level.size, sdflt.level.size);
  EXPECT_NE(snap.level.orderCount, sdflt.level.orderCount);

  const RecordHeader hdr = headerA(t);
  const RecordHeader hdflt;
  EXPECT_NE(hdr.tsEventNs, hdflt.tsEventNs);
  EXPECT_NE(hdr.tsRecvNs, hdflt.tsRecvNs);
  EXPECT_NE(hdr.providerSeq, hdflt.providerSeq);
  EXPECT_NE(hdr.sessionId, hdflt.sessionId);
  EXPECT_NE(hdr.symbolId, hdflt.symbolId);
  EXPECT_NE(hdr.venueId, hdflt.venueId);
  EXPECT_NE(hdr.instrumentId, hdflt.instrumentId);
}

// ---------------------------------------------------------------------------
// The round trips
// ---------------------------------------------------------------------------

TEST(RoundTripTest, Trades) {
  Tables t;
  const auto in = tradeFixtures(t);

  auto batch = encodeTrades(in, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  ASSERT_EQ((*batch)->num_rows(), static_cast<std::int64_t>(in.size()));
  EXPECT_TRUE((*batch)->schema()->Equals(*tradesSchema(), false));
  ASSERT_TRUE((*batch)->Validate().ok()) << (*batch)->Validate().ToString();

  // Decode into FRESH tables: the ids must be reconstructed from the
  // dictionary text, not inherited. If the ids happened to match only because
  // the same table was reused, this test would prove nothing about the wire.
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  ASSERT_TRUE(decodeTicks(**batch, RecordKind::Trade, symbols, venues, out).ok());
  ASSERT_EQ(out.size(), in.size());

  for (std::size_t i = 0; i < in.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    EXPECT_EQ(out[i].kind, RecordKind::Trade);
    EXPECT_EQ(out[i].price, in[i].price);
    EXPECT_EQ(out[i].size, in[i].size);
    EXPECT_EQ(out[i].aggressor, in[i].aggressor);
    EXPECT_EQ(out[i].bookNeutral, in[i].bookNeutral);
    EXPECT_EQ(out[i].tradeId, in[i].tradeId);
    EXPECT_EQ(out[i].flags, in[i].flags);

    // Header fields other than the interned ids are value-identical.
    EXPECT_EQ(out[i].header.tsEventNs, in[i].header.tsEventNs);
    EXPECT_EQ(out[i].header.tsRecvNs, in[i].header.tsRecvNs);
    EXPECT_EQ(out[i].header.providerSeq, in[i].header.providerSeq);
    EXPECT_EQ(out[i].header.sessionId, in[i].header.sessionId);
    EXPECT_EQ(out[i].header.instrumentId, in[i].header.instrumentId);

    // The ids are new, but they must name the SAME TEXT. This is the actual
    // interning contract: the id is table-local, the ticker is the identity.
    EXPECT_EQ(symbols.name(out[i].header.symbolId),
              t.symbols.name(in[i].header.symbolId));
    EXPECT_EQ(venues.name(out[i].header.venueId),
              t.venues.name(in[i].header.venueId));
  }

  // Interned once per distinct value, not once per row (docs §5.3): three
  // rows, two symbols, two venues.
  EXPECT_EQ(symbols.size(), 2u);
  EXPECT_EQ(venues.size(), 2u);
}

TEST(RoundTripTest, Quotes) {
  Tables t;
  const auto in = quoteFixtures(t);
  auto batch = encodeQuotes(in, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  EXPECT_TRUE((*batch)->schema()->Equals(*quotesSchema(), false));
  ASSERT_TRUE((*batch)->Validate().ok());

  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  ASSERT_TRUE(decodeTicks(**batch, RecordKind::Quote, symbols, venues, out).ok());
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    EXPECT_EQ(out[i].kind, RecordKind::Quote);
    EXPECT_EQ(out[i].bidPx, in[i].bidPx);
    EXPECT_EQ(out[i].askPx, in[i].askPx);
    EXPECT_EQ(out[i].bidSz, in[i].bidSz);
    EXPECT_EQ(out[i].askSz, in[i].askSz);
    EXPECT_EQ(out[i].bidCt, in[i].bidCt);
    EXPECT_EQ(out[i].askCt, in[i].askCt);
    EXPECT_EQ(out[i].flags, in[i].flags);
    // Interned ids are table-local, so compare them by the text they name and
    // the rest of the header by value.
    RecordHeader decoded = out[i].header;
    EXPECT_EQ(symbols.name(decoded.symbolId),
              t.symbols.name(in[i].header.symbolId));
    EXPECT_EQ(venues.name(decoded.venueId),
              t.venues.name(in[i].header.venueId));
    decoded.symbolId = in[i].header.symbolId;
    decoded.venueId = in[i].header.venueId;
    expectHeaderEq(decoded, in[i].header);
  }
  // kNoPrice is INT64_MIN and must survive as itself, not collapse to 0.
  EXPECT_EQ(out[1].bidPx, kNoPrice);
}

TEST(RoundTripTest, BookEvents) {
  Tables t;
  const auto in = bookFixtures(t);
  auto batch = encodeBookEvents(in, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  EXPECT_TRUE((*batch)->schema()->Equals(*bookEventsSchema(), false));
  ASSERT_TRUE((*batch)->Validate().ok());

  SymbolTable symbols, venues;
  std::vector<BookRecord> out;
  ASSERT_TRUE(decodeBookEvents(**batch, symbols, venues, out).ok());
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    EXPECT_EQ(out[i].action, in[i].action);
    EXPECT_EQ(out[i].side, in[i].side);
    EXPECT_EQ(out[i].price, in[i].price);
    EXPECT_EQ(out[i].size, in[i].size);
    EXPECT_EQ(out[i].orderId, in[i].orderId);
    EXPECT_EQ(out[i].priority, in[i].priority);
    EXPECT_EQ(out[i].depth, in[i].depth);
    EXPECT_EQ(out[i].epoch, in[i].epoch);
    EXPECT_EQ(out[i].flags, in[i].flags);
    EXPECT_EQ(out[i].header.tsEventNs, in[i].header.tsEventNs);
    EXPECT_EQ(out[i].header.providerSeq, in[i].header.providerSeq);
    EXPECT_EQ(symbols.name(out[i].header.symbolId),
              t.symbols.name(in[i].header.symbolId));
  }
}

TEST(RoundTripTest, BookSnapshots) {
  Tables t;
  const auto in = snapshotFixtures(t);
  auto batch = encodeBookSnapshots(in, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  EXPECT_TRUE((*batch)->schema()->Equals(*bookSnapshotsSchema(), false));
  ASSERT_TRUE((*batch)->Validate().ok());

  SymbolTable symbols, venues;
  std::vector<SnapshotRecord> out;
  ASSERT_TRUE(decodeBookSnapshots(**batch, symbols, venues, out).ok());
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    EXPECT_EQ(out[i].snapshotSeq, in[i].snapshotSeq);
    EXPECT_EQ(out[i].level.side, in[i].level.side);
    EXPECT_EQ(out[i].level.price, in[i].level.price);
    EXPECT_EQ(out[i].level.size, in[i].level.size);
    EXPECT_EQ(out[i].level.orderCount, in[i].level.orderCount);
    EXPECT_EQ(out[i].header.tsEventNs, in[i].header.tsEventNs);
  }
  // Two rows sharing one snapshotSeq is what makes them one snapshot.
  EXPECT_EQ(out[0].snapshotSeq, out[1].snapshotSeq);
}

// A batch with no rows must still produce the canonical schema and decode to
// nothing — the empty-window case that every replay will hit at an edge.
TEST(RoundTripTest, EmptyBatchIsStillCanonical) {
  Tables t;
  auto batch = encodeTrades({}, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  EXPECT_EQ((*batch)->num_rows(), 0);
  EXPECT_TRUE((*batch)->schema()->Equals(*tradesSchema(), false));
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  ASSERT_TRUE(decodeTicks(**batch, RecordKind::Trade, symbols, venues, out).ok());
  EXPECT_TRUE(out.empty());
}

TEST(RoundTripTest, DecodeAppendsRatherThanClearing) {
  Tables t;
  const auto in = tradeFixtures(t);
  auto batch = encodeTrades(in, t.symbols, t.venues);
  ASSERT_TRUE(batch.ok());
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  ASSERT_TRUE(decodeTicks(**batch, RecordKind::Trade, symbols, venues, out).ok());
  ASSERT_TRUE(decodeTicks(**batch, RecordKind::Trade, symbols, venues, out).ok());
  EXPECT_EQ(out.size(), in.size() * 2);
  // Second pass must reuse the ids the first pass interned, not grow the table.
  EXPECT_EQ(symbols.size(), 2u);
}

// ---------------------------------------------------------------------------
// Refusals — the codec must fail loudly, not plausibly
// ---------------------------------------------------------------------------

TEST(CodecRefusalTest, DecodingTheWrongTableFails) {
  Tables t;
  auto book = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(book.ok());
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  // Both tables have `price` and `size` at compatible types, so a
  // column-by-name decode without a schema check would read `action` rows as
  // trades and return numbers.
  const auto st = decodeTicks(**book, RecordKind::Trade, symbols, venues, out);
  EXPECT_FALSE(st.ok());
  EXPECT_TRUE(out.empty());
}

TEST(CodecRefusalTest, EncodingAnUninternedIdFails) {
  Tables t;
  auto in = tradeFixtures(t);
  in[0].header.symbolId = 9'999;   // in no table
  const auto batch = encodeTrades(in, t.symbols, t.venues);
  EXPECT_FALSE(batch.ok());
  // Specifically NOT an empty ticker: a blank symbol in L1 is
  // indistinguishable from a provider gap (the confusion SPEC D11 exists to
  // prevent one level up), so the encoder refuses rather than writing "".
  EXPECT_NE(batch.status().ToString().find("not interned"), std::string::npos)
      << batch.status().ToString();
}

TEST(CodecRefusalTest, MixedKindsInOneVectorFails) {
  Tables t;
  auto in = tradeFixtures(t);
  in[1].kind = RecordKind::Quote;
  EXPECT_FALSE(encodeTrades(in, t.symbols, t.venues).ok());
}

TEST(CodecRefusalTest, DecodeTicksRejectsBookKinds) {
  Tables t;
  auto batch = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok());
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  EXPECT_FALSE(decodeTicks(**batch, RecordKind::BookEvent, symbols, venues, out).ok());
  EXPECT_FALSE(decodeTicks(**batch, RecordKind::BookSnapshot, symbols, venues, out).ok());
}

TEST(CodecRefusalTest, AnUnknownSchemaVersionIsRefused) {
  Tables t;
  auto batch = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok());
  // Same columns, future version. A reader that meets an unknown version must
  // refuse the stream rather than guess a column layout.
  auto future = (*batch)->schema()->WithMetadata(
      arrow::key_value_metadata({std::string(kSchemaVersionKey)}, {"99"}));
  auto relabelled = arrow::RecordBatch::Make(future, (*batch)->num_rows(),
                                             (*batch)->columns());
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  const auto st = decodeTicks(*relabelled, RecordKind::Trade, symbols, venues, out);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.ToString().find("99"), std::string::npos) << st.ToString();
}

// ---------------------------------------------------------------------------
// IPC — the only reason ARROW_IPC is ON (ENC-1485 measures FORMAT ArrowStream)
// ---------------------------------------------------------------------------

namespace {

// One Arrow IPC stream, written and read back. `compression` is
// UNCOMPRESSED for the plain stream and a real codec otherwise —
// `Codec::Create` fails outright when the codec was not built, so a codec case
// here is a genuine assertion about the build and not about Arrow's enum.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> ipcRoundTrip(
    const arrow::RecordBatch& batch, arrow::Compression::type compression) {
  auto options = arrow::ipc::IpcWriteOptions::Defaults();
  if (compression != arrow::Compression::UNCOMPRESSED) {
    ARROW_ASSIGN_OR_RAISE(auto codec, arrow::util::Codec::Create(compression));
    options.codec = std::shared_ptr<arrow::util::Codec>(std::move(codec));
  }
  ARROW_ASSIGN_OR_RAISE(auto sink, arrow::io::BufferOutputStream::Create());
  ARROW_ASSIGN_OR_RAISE(
      auto writer, arrow::ipc::MakeStreamWriter(sink, batch.schema(), options));
  ARROW_RETURN_NOT_OK(writer->WriteRecordBatch(batch));
  ARROW_RETURN_NOT_OK(writer->Close());
  ARROW_ASSIGN_OR_RAISE(auto buffer, sink->Finish());

  auto source = std::make_shared<arrow::io::BufferReader>(buffer);
  ARROW_ASSIGN_OR_RAISE(auto reader,
                        arrow::ipc::RecordBatchStreamReader::Open(source));
  // The schema metadata — and therefore kSchemaVersionKey — must survive the
  // stream, or requireCanonicalBatch() would reject every batch that came
  // over HTTP.
  if (schemaVersionOf(*reader->schema()) != kSchemaVersion) {
    return arrow::Status::Invalid(
        "gma.schema_version did not survive the IPC stream (got ",
        schemaVersionOf(*reader->schema()), ")");
  }
  std::shared_ptr<arrow::RecordBatch> got;
  ARROW_RETURN_NOT_OK(reader->ReadNext(&got));
  if (got == nullptr) {
    return arrow::Status::Invalid("the IPC stream held no record batch");
  }
  return got;
}

// Encode -> stream -> decode, for one table. All seven header columns and both
// dictionary columns are checked here for every table; `compare` adds that
// table's payload fields.
//
// What this does NOT assert: that the compressed bodies were actually
// compressed. Arrow gives no portable way to read a stream's body
// compression back, so the real assertion about the codecs is that
// `Codec::Create` succeeds at all -- it fails outright when the codec was not
// built, which is the thing GMA_DATA_ARROW_IPC_CODECS controls. If Arrow ever
// silently ignored IpcWriteOptions::codec, these cases would still pass.
template <typename Record, typename Encode, typename Decode, typename Compare>
void expectStreamRoundTrip(const Tables& t, const std::vector<Record>& in,
                           arrow::Compression::type compression,
                           Encode encode, Decode decode, Compare compare) {
  auto batch = encode(in);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  auto got = ipcRoundTrip(**batch, compression);
  ASSERT_TRUE(got.ok()) << got.status().ToString();

  SymbolTable symbols, venues;
  std::vector<Record> out;
  const auto st = decode(**got, symbols, venues, out);
  ASSERT_TRUE(st.ok()) << st.ToString();
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    SCOPED_TRACE("row " + std::to_string(i));
    // The dictionary columns are the part most likely to break in a stream:
    // the index widths differ between the two (int32 for symbol, int16 for
    // venue) and the dictionary travels as its own message ahead of the
    // record batch.
    EXPECT_EQ(symbols.name(out[i].header.symbolId),
              t.symbols.name(in[i].header.symbolId));
    EXPECT_EQ(venues.name(out[i].header.venueId),
              t.venues.name(in[i].header.venueId));
    EXPECT_EQ(out[i].header.tsEventNs, in[i].header.tsEventNs);
    EXPECT_EQ(out[i].header.tsRecvNs, in[i].header.tsRecvNs);
    EXPECT_EQ(out[i].header.providerSeq, in[i].header.providerSeq);
    EXPECT_EQ(out[i].header.sessionId, in[i].header.sessionId);
    EXPECT_EQ(out[i].header.instrumentId, in[i].header.instrumentId);
    compare(out[i], in[i]);
  }
}

}  // namespace

// ALL FOUR TABLES, AND COMPRESSED AS WELL AS PLAIN. This used to exercise
// `encodeBookEvents` alone under the name "EachTable", so trades, quotes and
// snapshots had no IPC coverage at all — a regression in the int32 symbol
// dictionary surviving a stream would have been caught by nothing. And nothing
// read a COMPRESSED stream, which is the sole stated reason
// GMA_DATA_ARROW_IPC_CODECS defaults ON: ClickHouse's
// `output_format_arrow_compression_method` defaults to lz4_frame, so the one
// thing that would break ENC-1485's transport was the untested one.
TEST(IpcTest, EveryTableSurvivesAnIpcStreamRoundTrip) {
  for (const auto compression : {arrow::Compression::UNCOMPRESSED,
                                 arrow::Compression::LZ4_FRAME,
                                 arrow::Compression::ZSTD}) {
    SCOPED_TRACE("compression=" + std::to_string(static_cast<int>(compression)));
    Tables t;

    expectStreamRoundTrip<TickRecord>(
        t, tradeFixtures(t), compression,
        [&t](const std::vector<TickRecord>& in) {
          return encodeTrades(in, t.symbols, t.venues);
        },
        [](const arrow::RecordBatch& b, SymbolTable& s, SymbolTable& v,
           std::vector<TickRecord>& o) {
          return decodeTicks(b, RecordKind::Trade, s, v, o);
        },
        [](const TickRecord& got, const TickRecord& want) {
          EXPECT_EQ(got.price, want.price);
          EXPECT_EQ(got.size, want.size);
          EXPECT_EQ(got.aggressor, want.aggressor);
          EXPECT_EQ(got.bookNeutral, want.bookNeutral);
          EXPECT_EQ(got.tradeId, want.tradeId);
          EXPECT_EQ(got.flags, want.flags);
        });

    expectStreamRoundTrip<TickRecord>(
        t, quoteFixtures(t), compression,
        [&t](const std::vector<TickRecord>& in) {
          return encodeQuotes(in, t.symbols, t.venues);
        },
        [](const arrow::RecordBatch& b, SymbolTable& s, SymbolTable& v,
           std::vector<TickRecord>& o) {
          return decodeTicks(b, RecordKind::Quote, s, v, o);
        },
        [](const TickRecord& got, const TickRecord& want) {
          EXPECT_EQ(got.bidPx, want.bidPx);
          EXPECT_EQ(got.askPx, want.askPx);
          EXPECT_EQ(got.bidSz, want.bidSz);
          EXPECT_EQ(got.askSz, want.askSz);
          EXPECT_EQ(got.bidCt, want.bidCt);
          EXPECT_EQ(got.askCt, want.askCt);
          EXPECT_EQ(got.flags, want.flags);
        });

    expectStreamRoundTrip<BookRecord>(
        t, bookFixtures(t), compression,
        [&t](const std::vector<BookRecord>& in) {
          return encodeBookEvents(in, t.symbols, t.venues);
        },
        &decodeBookEvents,
        [](const BookRecord& got, const BookRecord& want) {
          EXPECT_EQ(got.action, want.action);
          EXPECT_EQ(got.side, want.side);
          EXPECT_EQ(got.price, want.price);
          EXPECT_EQ(got.size, want.size);
          EXPECT_EQ(got.orderId, want.orderId);
          EXPECT_EQ(got.priority, want.priority);
          EXPECT_EQ(got.depth, want.depth);
          EXPECT_EQ(got.epoch, want.epoch);
          EXPECT_EQ(got.flags, want.flags);
        });

    expectStreamRoundTrip<SnapshotRecord>(
        t, snapshotFixtures(t), compression,
        [&t](const std::vector<SnapshotRecord>& in) {
          return encodeBookSnapshots(in, t.symbols, t.venues);
        },
        &decodeBookSnapshots,
        [](const SnapshotRecord& got, const SnapshotRecord& want) {
          EXPECT_EQ(got.snapshotSeq, want.snapshotSeq);
          EXPECT_EQ(got.level.side, want.level.side);
          EXPECT_EQ(got.level.price, want.level.price);
          EXPECT_EQ(got.level.size, want.level.size);
          EXPECT_EQ(got.level.orderCount, want.level.orderCount);
        });
  }
}

TEST(IpcTest, TheIpcCodecsClickHouseDefaultsToAreActuallyBuilt) {
  // GMA_DATA_ARROW_IPC_CODECS defaults ON for exactly one reason: a codec-free
  // Arrow cannot read ENC-1485's stream at all, because ClickHouse compresses
  // ArrowStream output with lz4_frame unless told otherwise. Asserted against
  // the built library rather than inferred from the CMake option, which is
  // what "the link wiring is correct but unexercised" meant.
  for (const auto compression : {arrow::Compression::LZ4_FRAME,
                                 arrow::Compression::ZSTD}) {
    SCOPED_TRACE("compression=" + std::to_string(static_cast<int>(compression)));
    EXPECT_TRUE(arrow::util::Codec::IsAvailable(compression));
    auto codec = arrow::util::Codec::Create(compression);
    EXPECT_TRUE(codec.ok()) << codec.status().ToString();
  }
}

// ---------------------------------------------------------------------------
// Wire-input validation — content the schema check CANNOT catch
// ---------------------------------------------------------------------------
//
// Everything above this point feeds the decoder something the encoder made.
// A batch off `FORMAT ArrowStream` is not: its column types can be exactly
// canonical while its contents are not representable, and every case below is
// one where the decode used to return Status::OK() and hand back a record that
// is wrong rather than absent.

TEST(CodecRefusalTest, EnumDomainBoundsMatchRecordHpp) {
  // The codec bounds enum columns against the LAST enumerator of each enum
  // (kMaxSideValue &c. in Schema.cpp). This pins the values those bounds
  // resolve to today, so a reordering that moves the largest enumerator is
  // visible here rather than only in behaviour.
  EXPECT_EQ(static_cast<std::uint8_t>(Side::Ask), 1);
  EXPECT_EQ(static_cast<std::uint8_t>(Aggressor::Sell), 2);
  EXPECT_EQ(static_cast<std::uint8_t>(BookAction::Clear), 5);
}

// THE DRIFT GUARD, and the reason it is written out longhand.
//
// C++ cannot ask an enum for its largest enumerator, so the codec's bounds are
// maintained by hand. If someone adds `BookAction::NewThing = 6` and forgets
// the bound, the codec starts refusing a LEGAL value — and every refusal test
// stays green, because they all assert that out-of-domain bytes are rejected.
// The only thing that catches that direction is a list of the enumerators the
// codec must ACCEPT. Listing them one per line is deliberate: a range loop
// would silently cover a new enumerator and assert nothing about the bound,
// whereas this list is a thing the person adding an enumerator has to come and
// extend, at which point the test fails and names the bound.
TEST(CodecRefusalTest, DecodeAcceptsEveryDeclaredEnumerator) {
  Tables t;
  SymbolTable symbols, venues;

  const std::vector<BookAction> allActions = {
      BookAction::Add, BookAction::Update, BookAction::Delete,
      BookAction::Trade, BookAction::TickSize, BookAction::Clear,
  };
  const std::vector<Side> allSides = {Side::Bid, Side::Ask};
  const std::vector<Aggressor> allAggressors = {
      Aggressor::Unknown, Aggressor::Buy, Aggressor::Sell,
  };

  auto book = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(book.ok()) << book.status().ToString();
  for (const auto action : allActions) {
    SCOPED_TRACE("action=" + std::to_string(static_cast<int>(action)));
    const int v = static_cast<int>(action);
    auto b = withColumnReplaced(**book, "action", uint8Column({v, v, v}));
    std::vector<BookRecord> out;
    const auto st = decodeBookEvents(*b, symbols, venues, out);
    EXPECT_TRUE(st.ok())
        << st.ToString()
        << "\n  a DECLARED BookAction was refused: raise kMaxBookActionValue "
           "in data/src/Schema.cpp to the new last enumerator";
    if (st.ok()) {
      ASSERT_EQ(out.size(), 3u);
      EXPECT_EQ(out[0].action, action);
    }
    out.clear();
  }
  for (const auto side : allSides) {
    SCOPED_TRACE("side=" + std::to_string(static_cast<int>(side)));
    const int v = static_cast<int>(side);
    auto b = withColumnReplaced(**book, "side", uint8Column({v, v, v}));
    std::vector<BookRecord> out;
    const auto st = decodeBookEvents(*b, symbols, venues, out);
    EXPECT_TRUE(st.ok()) << st.ToString()
                         << "\n  a DECLARED Side was refused: raise "
                            "kMaxSideValue in data/src/Schema.cpp";
  }

  auto trades = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(trades.ok()) << trades.status().ToString();
  for (const auto aggressor : allAggressors) {
    SCOPED_TRACE("aggressor=" + std::to_string(static_cast<int>(aggressor)));
    const int v = static_cast<int>(aggressor);
    auto b = withColumnReplaced(**trades, "aggressor", uint8Column({v, v, v}));
    std::vector<TickRecord> out;
    const auto st = decodeTicks(*b, RecordKind::Trade, symbols, venues, out);
    EXPECT_TRUE(st.ok()) << st.ToString()
                         << "\n  a DECLARED Aggressor was refused: raise "
                            "kMaxAggressorValue in data/src/Schema.cpp";
  }
}

TEST(CodecRefusalTest, AnOutOfDomainBookActionOrSideIsRefused) {
  Tables t;
  auto batch = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  SymbolTable symbols, venues;

  // uint8 is a legal column type for all 256 byte values, so the schema check
  // passes and the batch is indistinguishable from a good one by type alone.
  // 9 is not a BookAction; a BookRecord carrying it matches no enumerator and
  // §4.6's dispatch onto OrderBookManager would drop the event in silence.
  {
    auto bad = withColumnReplaced(**batch, "action", uint8Column({0, 9, 4}));
    std::vector<BookRecord> out;
    const auto st = decodeBookEvents(*bad, symbols, venues, out);
    EXPECT_FALSE(st.ok()) << "action=9 decoded without complaint";
    EXPECT_NE(st.ToString().find("action"), std::string::npos) << st.ToString();
  }
  // One past the last enumerator, which is the case a new BookAction would
  // land on: if BookAction grows, this reddens and sends you to the bound in
  // Schema.cpp rather than shipping a silently-dropped action.
  {
    auto bad = withColumnReplaced(**batch, "action", uint8Column({0, 6, 4}));
    std::vector<BookRecord> out;
    EXPECT_FALSE(decodeBookEvents(*bad, symbols, venues, out).ok());
  }
  {
    auto bad = withColumnReplaced(**batch, "side", uint8Column({0, 7, 1}));
    std::vector<BookRecord> out;
    const auto st = decodeBookEvents(*bad, symbols, venues, out);
    EXPECT_FALSE(st.ok()) << "side=7 decoded without complaint";
    EXPECT_NE(st.ToString().find("side"), std::string::npos) << st.ToString();
  }
  // The legal boundary value still decodes — the check must be a domain check,
  // not a narrower one.
  {
    auto ok = withColumnReplaced(**batch, "action", uint8Column({5, 5, 5}));
    std::vector<BookRecord> out;
    EXPECT_TRUE(decodeBookEvents(*ok, symbols, venues, out).ok());
  }
}

TEST(CodecRefusalTest, AnOutOfDomainAggressorOrSnapshotSideIsRefused) {
  Tables t;
  auto trades = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(trades.ok()) << trades.status().ToString();
  SymbolTable symbols, venues;
  {
    auto bad = withColumnReplaced(**trades, "aggressor", uint8Column({1, 3, 2}));
    std::vector<TickRecord> out;
    const auto st = decodeTicks(*bad, RecordKind::Trade, symbols, venues, out);
    EXPECT_FALSE(st.ok()) << "aggressor=3 decoded without complaint";
    EXPECT_NE(st.ToString().find("aggressor"), std::string::npos) << st.ToString();
  }

  auto snaps = encodeBookSnapshots(snapshotFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(snaps.ok()) << snaps.status().ToString();
  {
    auto bad = withColumnReplaced(**snaps, "side", uint8Column({0, 1, 2}));
    std::vector<SnapshotRecord> out;
    const auto st = decodeBookSnapshots(*bad, symbols, venues, out);
    EXPECT_FALSE(st.ok()) << "side=2 decoded without complaint";
    EXPECT_NE(st.ToString().find("side"), std::string::npos) << st.ToString();
  }
}

TEST(CodecRefusalTest, ABatchWithAShortPayloadColumnIsRefused) {
  // THE SHARP CASE, and the reason the num_rows variant below is not evidence
  // on its own. Here the seven header columns are full length, so hdr.bind()
  // and dictIdAt() both succeed and nothing incidental goes wrong: only the
  // payload column is short. Unchecked, the decode returns Status::OK() and
  // two of the three records carry a price read from past the end of the Arrow
  // buffer. (In the num_rows case the dictionary index read happens to land
  // out of range, so an unchecked decode errors for the WRONG reason — having
  // already performed the out-of-bounds read.)
  Tables t;
  SymbolTable symbols, venues;
  auto trades = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(trades.ok()) << trades.status().ToString();
  ASSERT_EQ((*trades)->num_rows(), 3);
  auto shortPrice = withColumnReplaced(**trades, "price", int64Column({7}));
  std::vector<TickRecord> out;
  const auto st = decodeTicks(*shortPrice, RecordKind::Trade, symbols, venues, out);
  EXPECT_FALSE(st.ok())
      << "a 1-row price column in a 3-row batch decoded without complaint; "
         "rows 1 and 2 were read out of bounds";
}

TEST(CodecRefusalTest, ABatchWhoseNumRowsExceedsItsColumnsIsRefused) {
  // `arrow::RecordBatch::Make` does not validate and the IPC reader does not
  // run ValidateFull, so a batch whose num_rows exceeds its columns' length is
  // constructible — a truncated ArrowStream, or a caller assembling one. Every
  // decoder loops to num_rows, so unchecked this is `price->Value(i)` reading
  // past the Arrow buffer: an out-of-bounds READ, not a Status. The decoders
  // now call batch.Validate() before the first Value().
  Tables t;
  SymbolTable symbols, venues;

  auto trades = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(trades.ok()) << trades.status().ToString();
  auto lyingTrades = arrow::RecordBatch::Make(
      (*trades)->schema(), (*trades)->num_rows() + 4096, (*trades)->columns());
  std::vector<TickRecord> tickOut;
  EXPECT_FALSE(
      decodeTicks(*lyingTrades, RecordKind::Trade, symbols, venues, tickOut).ok());

  auto book = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(book.ok()) << book.status().ToString();
  auto lyingBook = arrow::RecordBatch::Make(
      (*book)->schema(), (*book)->num_rows() + 4096, (*book)->columns());
  std::vector<BookRecord> bookOut;
  EXPECT_FALSE(decodeBookEvents(*lyingBook, symbols, venues, bookOut).ok());

  auto snaps = encodeBookSnapshots(snapshotFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(snaps.ok()) << snaps.status().ToString();
  auto lyingSnaps = arrow::RecordBatch::Make(
      (*snaps)->schema(), (*snaps)->num_rows() + 4096, (*snaps)->columns());
  std::vector<SnapshotRecord> snapOut;
  EXPECT_FALSE(decodeBookSnapshots(*lyingSnaps, symbols, venues, snapOut).ok());
}

TEST(CodecRefusalTest, ANullInADictionaryValueIsRefused) {
  // The one a column-level null_count() cannot see: Array::null_count() on a
  // DictionaryArray counts nulls in its INDICES, so a null in the dictionary's
  // VALUES array is invisible to it. Unchecked, GetView() on that slot yields
  // "" and SymbolTable::intern("") issues an ordinary id (pinned by
  // SymbolTableTest.EmptyStringGetsAnOrdinaryId) — so the record names a real
  // symbol called "", with Status::OK(). That is the exact confusion the
  // ENCODER refuses an un-interned id to avoid, arriving from the other end.
  Tables t;
  auto batch = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  const auto& dict = static_cast<const arrow::DictionaryArray&>(
      *(*batch)->GetColumnByName("symbol"));

  const auto& originalValues =
      static_cast<const arrow::StringArray&>(*dict.dictionary());
  arrow::StringBuilder values;
  ASSERT_TRUE(values.AppendNull().ok());          // was "AAPL"
  for (std::int64_t i = 1; i < originalValues.length(); ++i) {
    ASSERT_TRUE(values.Append(originalValues.GetView(i)).ok());
  }
  std::shared_ptr<arrow::Array> nulledValues;
  ASSERT_TRUE(values.Finish(&nulledValues).ok());
  auto poisoned = arrow::DictionaryArray::FromArrays(
      dict.type(), dict.indices(), nulledValues);
  ASSERT_TRUE(poisoned.ok()) << poisoned.status().ToString();
  // The column-level null count still reads zero — which is why this needed
  // its own check rather than the generic one.
  EXPECT_EQ((*poisoned)->null_count(), 0);

  auto bad = withColumnReplaced(**batch, "symbol", *poisoned);
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  const auto st = decodeTicks(*bad, RecordKind::Trade, symbols, venues, out);
  EXPECT_FALSE(st.ok()) << "a null ticker decoded into an interned \"\"";
  EXPECT_NE(st.ToString().find("null"), std::string::npos) << st.ToString();
}

TEST(CodecRefusalTest, ARefusedBatchAppendsNothing) {
  // `out` is append-only across batches (Schema.hpp), which only works if a
  // refused batch contributes nothing: a caller that accumulates and checks
  // the Status at the end would otherwise be holding rows [0, i) of a batch
  // the codec rejected. The enum and dictionary checks make mid-loop refusal
  // routine, so this is the normal path, not an edge case.
  Tables t;
  SymbolTable symbols, venues;

  // Seed with one good batch so the test distinguishes "rolled back to entry"
  // from "cleared".
  std::vector<BookRecord> out;
  auto good = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(good.ok()) << good.status().ToString();
  ASSERT_TRUE(decodeBookEvents(**good, symbols, venues, out).ok());
  const std::size_t accumulated = out.size();
  ASSERT_EQ(accumulated, 3u);

  // Row 0 is legal, row 1 is not: an unguarded decode leaves row 0 behind.
  auto bad = withColumnReplaced(**good, "action", uint8Column({0, 9, 4}));
  const auto st = decodeBookEvents(*bad, symbols, venues, out);
  ASSERT_FALSE(st.ok()) << "the fixture for this test is not being refused";
  EXPECT_EQ(out.size(), accumulated)
      << "a refused batch left " << (out.size() - accumulated)
      << " partially-decoded row(s) in the caller's vector";

  std::vector<TickRecord> ticks;
  auto trades = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(trades.ok()) << trades.status().ToString();
  auto badTrades = withColumnReplaced(**trades, "aggressor",
                                      uint8Column({1, 3, 2}));
  ASSERT_FALSE(
      decodeTicks(*badTrades, RecordKind::Trade, symbols, venues, ticks).ok());
  EXPECT_TRUE(ticks.empty()) << ticks.size() << " row(s) survived a refusal";

  std::vector<SnapshotRecord> snaps;
  auto snapBatch = encodeBookSnapshots(snapshotFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(snapBatch.ok()) << snapBatch.status().ToString();
  auto badSnaps = withColumnReplaced(**snapBatch, "side", uint8Column({0, 1, 2}));
  ASSERT_FALSE(decodeBookSnapshots(*badSnaps, symbols, venues, snaps).ok());
  EXPECT_TRUE(snaps.empty()) << snaps.size() << " row(s) survived a refusal";
}

TEST(CodecRefusalTest, ANullInANonNullableColumnIsRefused) {
  // Every column of every canonical schema is non-nullable and the PODs have
  // no absent state, so a null has no representation. Unchecked, `Value(i)` on
  // a null slot returns a plausible zero — a price of 0 is legal, so this is
  // the "fails plausibly rather than loudly" mode again.
  Tables t;
  auto batch = encodeTrades(tradeFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  auto bad = withColumnReplaced(**batch, "price", int64ColumnWithANull());
  SymbolTable symbols, venues;
  std::vector<TickRecord> out;
  const auto st = decodeTicks(*bad, RecordKind::Trade, symbols, venues, out);
  EXPECT_FALSE(st.ok()) << "a null price decoded into a TickRecord";
}
