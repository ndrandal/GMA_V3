#pragma once
//
// Canonical engine-facing market-data records (ENC-1481).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D3, D8;
//         docs/data-layer.md §5.3, §8.3.
//
// WHY THIS FILE IS UNDER include/ AND NOT UNDER data/
// ---------------------------------------------------
// `gma_data` depends on `gma_engine`; `gma_engine` depends on neither it nor
// any connector, and **Arrow and the ClickHouse client must never become
// engine dependencies** (SPEC D14, docs/data-layer.md §4 rule 6) — the
// live-only variant of D14's client profile omits `gma_data` entirely and must
// still link. (D14's other client variant reads a central L1 over the wire, so
// it does ship `gma_data`; the invariant constrains `gma_engine`, not the
// deployment.)
//
// But the engine has to be able to *name* a record: ENC-1486 adds
// `Dispatcher::onRecord(const TickRecord&)`, and `Dispatcher` is engine code.
// So the split is:
//
//   include/gma/data/Record.hpp   (THIS FILE, in gma_engine)
//       The PODs. Arrow-free, ClickHouse-free, dependency-free — <cstdint>
//       and nothing else. Plain aggregates of fixed-width fields.
//
//   data/include/gma/data/Schema.hpp   (in gma_data)
//       The arrow::Schema for each table, and the column-wise codec between
//       a RecordBatch and these PODs. That is where Arrow is linked and where
//       it stops.
//
// Both halves live in namespace `gma::data`, which is deliberate: the type is
// one type, in one namespace, with one definition. The library boundary is a
// *dependency* boundary, not a naming one.
//
// PRICES ARE int64 AT A FIXED GLOBAL SCALE (kPriceScale, 1e-9), matching
// Databento DBN exactly (SPEC D8). NOT per-symbol: `OrderBookManager`'s
// per-symbol tick size (setTickSize, default 1e-4) stays a book-quantization
// concern and does not reach the schema. Consequence: no per-row scale column,
// no per-symbol lookup on decode, and an exact integer path from DBN with no
// double round trip.
//
// SYMBOLS ARE INTERNED uint32 IDS, NOT STRINGS (SPEC D3). `StreamValue`
// carries `std::string symbol`, so today a symbol is heap-copied into every
// value on every edge of every DAG. For a chart tick that is irrelevant; at
// MBO rates (tens of millions of records per symbol-day) that single copy is a
// measurable fraction of total cost — which is what ENC-1487 (B2) measures.
// Resolve an id back to text only at an output boundary, through
// `gma::data::SymbolTable` (include/gma/data/SymbolTable.hpp).
//

#include <cstdint>

namespace gma::data {

// ---------------------------------------------------------------------------
// Fixed point
// ---------------------------------------------------------------------------

// Price scale denominator. A price of 1.234567890 is stored as 1234567890.
// Compile-time constant on purpose — see the header comment.
inline constexpr std::int64_t kPriceScale = 1'000'000'000LL;  // 1e-9

// Sentinel for "no price" (an empty book side, a quote with one side only).
// INT64_MIN rather than 0, because 0 is a legal price and negative prices are
// legal too (OrderBookManager::setAllowNegativePrices exists for bonds).
inline constexpr std::int64_t kNoPrice = INT64_MIN;

constexpr double toDouble(std::int64_t fixed) noexcept {
  return static_cast<double>(fixed) / static_cast<double>(kPriceScale);
}

// Rounds half away from zero. The normalizer must prefer an exact integer path
// from the provider's own fixed point (DBN is already 1e-9) and use this only
// for a provider that hands over a double.
std::int64_t toFixed(double px) noexcept;

// ---------------------------------------------------------------------------
// Enumerations — these MIRROR existing engine/connector enums rather than
// replacing them, so the decode is a cast and not a lookup table.
// ---------------------------------------------------------------------------

// Mirrors gma::Side (connectors/market/include/gma/book/OrderBook.hpp).
enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

// Mirrors gma::Aggressor (same header).
enum class Aggressor : std::uint8_t { Unknown = 0, Buy = 1, Sell = 2 };

// Mirrors the gma::feed::FeedEvent variant set
// (connectors/market/include/gma/feed/FeedEvent.hpp) one-for-one. That variant
// is already the canonical event model in this repo; the schema does not invent
// a second one.
enum class BookAction : std::uint8_t {
  Add      = 0,   // ObAddEvent
  Update   = 1,   // ObUpdateEvent
  Delete   = 2,   // ObDeleteEvent
  Trade    = 3,   // ObTradeEvent (print; see bookNeutral)
  TickSize = 4,   // ObTickSizeEvent
  Clear    = 5,   // ObResetEvent — new epoch
};

enum class RecordKind : std::uint8_t {
  Trade        = 0,
  Quote        = 1,   // L1 top-of-book
  BookEvent    = 2,   // L2/L3 delta
  BookSnapshot = 3,
};

// ---------------------------------------------------------------------------
// Engine-facing PODs
// ---------------------------------------------------------------------------

// Columns shared by every record, as the engine sees them. Mirrors
// docs/data-layer.md §8.3's "common columns" one-for-one, so a row of any L1
// table decodes into (header, payload) with nothing left over.
struct RecordHeader {
  // Venue event time. THE ONLY TIME AN OPERATOR MAY KEY OFF (SPEC D9).
  std::int64_t  tsEventNs   = 0;
  // Normalizer receipt. DIAGNOSTICS AND LATENCY ONLY — never an operator
  // input, never a window boundary (docs/data-layer.md §8.3).
  std::int64_t  tsRecvNs    = 0;
  // Monotonic within (sessionId, symbolId); synthesized by the normalizer when
  // the provider has none. Non-null is the point: the splice's dedup predicate
  // has no "what if absent" branch.
  std::uint64_t providerSeq = 0;
  // Normalizer session. Makes providerSeq comparable across a restart; the
  // dedup key is the PAIR (sessionId, providerSeq).
  std::uint32_t sessionId   = 0;

  // Interned ids, not strings — see the header comment. 0 is
  // kInvalidSymbolId: a default-constructed header names no symbol and no
  // venue, and can never be mistaken for a real one.
  std::uint32_t symbolId    = 0;
  std::uint32_t venueId     = 0;

  // The DBN instrument_id, retained for provenance (SPEC D7). It is
  // dataset-scoped and reassignable, so it is NOT a key and NOT an operator
  // input; `gma.symbology` holds the mapping. Carried here because it is free
  // — without it this struct is 36 bytes padded to 40 — and because dropping
  // it would make the Arrow round trip lossy.
  std::uint32_t instrumentId = 0;
};

// The provenance field above is free only if that stays true.
static_assert(sizeof(RecordHeader) == 40,
              "RecordHeader grew: instrumentId was justified as padding-free");

// A trade or an L1 quote, flattened. One struct for both because the engine
// consumes them identically (both update AtomicStore scalars and both clock
// listeners); `kind` says which fields are meaningful.
//
// Replaces, on the Arrow path, the gma::Event{symbol, shared_ptr<Document>,
// type} the JSON path uses. Field resolution happened in the normalizer, so
// there is no MarketFieldMap lookup and no HasMember() probe per tick.
struct TickRecord {
  RecordHeader header;
  RecordKind   kind = RecordKind::Trade;

  // kind == Trade
  std::int64_t  price       = kNoPrice;
  std::uint64_t size        = 0;
  Aggressor     aggressor   = Aggressor::Unknown;
  // Mirrors ObTradeEvent::bookNeutral: a print that must NOT consume resting
  // liquidity (hidden / non-displayable executions, and executions whose book
  // effect already arrived as an explicit add/update/delete). Dropping this
  // double-counts volume against the book.
  bool          bookNeutral = false;
  std::uint64_t tradeId     = 0;

  // kind == Quote. NOTE: `quotes` is not populated in the first slice — there
  // is no mbp-1 in the Databento plan, so top-of-book is computed from the
  // reconstructed book (docs/data-layer.md §8.4). The shape is defined so
  // feed-simulator and future L1-only providers have somewhere to land.
  std::int64_t  bidPx = kNoPrice;
  std::int64_t  askPx = kNoPrice;
  std::uint64_t bidSz = 0;
  std::uint64_t askSz = 0;
  std::uint32_t bidCt = 0;        // order count at the level, when provided
  std::uint32_t askCt = 0;

  std::uint16_t flags = 0;        // provider condition flags, passed through
};

// One L2/L3 book mutation. Maps onto the OrderBookManager entry points that
// already exist (onAdd / onUpdate / onDelete / onTrade / setTickSize /
// onReset), so the data layer drives that machinery rather than duplicating it
// (docs/data-layer.md §4.6).
struct BookRecord {
  RecordHeader  header;
  BookAction    action   = BookAction::Add;
  Side          side     = Side::Bid;
  std::int64_t  price    = kNoPrice;
  std::uint64_t size     = 0;
  std::uint64_t orderId  = 0;     // 0 == venue supplied none (OrderKey::idMissing)
  std::uint64_t priority = 0;     // recv seq or venue ts, per Order::priority
  std::uint16_t depth    = 0;     // level index for aggregated (L2) feeds
  std::uint32_t epoch    = 0;     // FeedScope::epoch; bumped by Clear
  std::uint16_t flags    = 0;
};

// One level of a book snapshot. Snapshots are persisted on a cadence so a
// replay of an arbitrary window can seek instead of rebuilding from session
// open — with MBO, the alternative is replaying every order event from the
// open, per research run (docs/data-layer.md §8.5).
struct SnapshotLevel {
  Side          side       = Side::Bid;
  std::int64_t  price      = kNoPrice;
  std::uint64_t size       = 0;
  std::uint32_t orderCount = 0;
};

// One ROW of `book_snapshots`: a level, plus the header, plus the resume seq
// the snapshot was taken at (`onSnapshotPerOrder`'s snapshotSeq). A snapshot is
// many rows sharing one snapshotSeq; the level alone is not addressable, which
// is why the row type exists rather than persisting bare SnapshotLevels.
struct SnapshotRecord {
  RecordHeader  header;
  std::uint64_t snapshotSeq = 0;
  SnapshotLevel level;
};

} // namespace gma::data
