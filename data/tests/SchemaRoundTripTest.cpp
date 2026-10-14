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
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <arrow/io/memory.h>
#include <arrow/util/key_value_metadata.h>
#include <arrow/record_batch.h>
#include <arrow/table.h>

#include <gtest/gtest.h>

#include <algorithm>
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
  // these ever compared equal, requireSchema() could not tell them apart and
  // decoding a book batch as trades would read `action` as a price.
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

TEST(IpcTest, EachTableSurvivesAnIpcStreamRoundTrip) {
  Tables t;
  auto batch = encodeBookEvents(bookFixtures(t), t.symbols, t.venues);
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();

  auto sink = arrow::io::BufferOutputStream::Create();
  ASSERT_TRUE(sink.ok());
  auto writer = arrow::ipc::MakeStreamWriter(*sink, (*batch)->schema());
  ASSERT_TRUE(writer.ok()) << writer.status().ToString();
  ASSERT_TRUE((*writer)->WriteRecordBatch(**batch).ok());
  ASSERT_TRUE((*writer)->Close().ok());
  auto buffer = (*sink)->Finish();
  ASSERT_TRUE(buffer.ok());

  auto source = std::make_shared<arrow::io::BufferReader>(*buffer);
  auto reader = arrow::ipc::RecordBatchStreamReader::Open(source);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();

  // The schema metadata — and therefore kSchemaVersionKey — must survive the
  // stream, or requireSchema() would reject every batch that came over HTTP.
  EXPECT_EQ(schemaVersionOf(*(*reader)->schema()), kSchemaVersion);

  std::shared_ptr<arrow::RecordBatch> got;
  ASSERT_TRUE((*reader)->ReadNext(&got).ok());
  ASSERT_NE(got, nullptr);

  SymbolTable symbols, venues;
  std::vector<BookRecord> out;
  ASSERT_TRUE(decodeBookEvents(*got, symbols, venues, out).ok());
  const auto in = bookFixtures(t);
  ASSERT_EQ(out.size(), in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    EXPECT_EQ(out[i].orderId, in[i].orderId) << "row " << i;
    EXPECT_EQ(out[i].price, in[i].price) << "row " << i;
    EXPECT_EQ(symbols.name(out[i].header.symbolId),
              t.symbols.name(in[i].header.symbolId)) << "row " << i;
  }
}
