#pragma once
//
// Canonical market-data schema — design sketch (ENC-1478, docs/data-layer.md §5).
// NOT IN THE BUILD. See data/README.md.
//
// Two representations, deliberately:
//
//   * arrow::Schema — the wire/storage form. Everything upstream of the engine
//     (adapter -> normalizer -> sink -> history -> replay) moves record BATCHES
//     in this shape. No row objects, no JSON DOM, no per-field conversion.
//
//   * TickRecord / BookRecord — plain PODs, the engine-facing form. ISource
//     decodes a batch into these COLUMN-WISE, once per batch, and hands them to
//     Dispatcher::onRecord. That decode is the single Arrow->scalar boundary in
//     the system (docs/data-layer.md C2). It exists because gma's DAG is a
//     scalar push pipeline — ArgType is a scalar variant and INode::onValue
//     takes one StreamValue — and making it columnar is a rewrite of ~25 node
//     types that this design explicitly does not take on.
//
// PRICES ARE int64 AT A FIXED GLOBAL SCALE (kPriceScale, 1e-9), matching
// Databento DBN. NOT per-symbol: OrderBookManager's per-symbol tick size
// (setTickSize, default 1e-4) stays a book-quantization concern and does not
// reach the schema. Consequence: no per-row scale column, no per-symbol lookup
// on decode. docs/data-layer.md C8 / Q4.
//

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

// Forward-declared so this header reads without an Arrow install.
namespace arrow { class Schema; class RecordBatch; }

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

// Rounds half away from zero. The normalizer should prefer an exact integer
// path from the provider's own fixed point (DBN is already 1e-9) and use this
// only for providers that hand over a double.
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
// Arrow schemas (docs/data-layer.md §5)
// ---------------------------------------------------------------------------
//
// Common columns on EVERY table:
//
//   ts_event      timestamp(NANO, "UTC")    venue event time. THE ONLY TIME AN
//                                           OPERATOR MAY KEY OFF.
//   ts_recv       timestamp(NANO, "UTC")    normalizer receipt. Diagnostics and
//                                           latency only — never an operator
//                                           input, and never a window boundary.
//   provider_seq  uint64, NON-NULL          monotonic within (session_id,
//                                           symbol). Synthesized by the
//                                           normalizer when the provider has
//                                           none (docs §4.4). Non-null is the
//                                           point: the splice's dedup predicate
//                                           (drop seq <= lastReplayedSeq) has no
//                                           "what if absent" branch.
//   session_id    uint32                    normalizer session. Makes
//                                           provider_seq comparable across a
//                                           restart; the dedup key is the PAIR
//                                           (session_id, provider_seq).
//   symbol        dictionary<int32, utf8>    thousands of values, millions of
//                                           rows.
//   venue         dictionary<int16, utf8>    Nasdaq / OTC / ...
//
// Three tables rather than one union table: a unified event table needs every
// type's fields nullable, which defeats the codecs and makes every read scan
// columns it does not want.

std::shared_ptr<arrow::Schema> tradesSchema();          // §5.2
std::shared_ptr<arrow::Schema> quotesSchema();          // §5.3  (L1)
std::shared_ptr<arrow::Schema> bookEventsSchema();      // §5.4  (L2/L3 deltas)
std::shared_ptr<arrow::Schema> bookSnapshotsSchema();   // §5.4

std::shared_ptr<arrow::Schema> schemaFor(RecordKind kind);

// Schema identity, written into every Arrow IPC file's and Parquet file's
// key-value metadata under "gma.schema_version". A reader that meets an
// unknown version refuses the file rather than guessing a column layout.
inline constexpr std::string_view kSchemaVersionKey = "gma.schema_version";
inline constexpr std::int32_t     kSchemaVersion    = 1;

// ---------------------------------------------------------------------------
// Engine-facing PODs — the output of the one decode boundary
// ---------------------------------------------------------------------------

// Columns shared by every record, as the engine sees them.
struct RecordHeader {
  std::int64_t  tsEventNs   = 0;        // event time. Drives every clock.
  std::int64_t  tsRecvNs    = 0;        // diagnostics only.
  std::uint64_t providerSeq = 0;
  std::uint32_t sessionId   = 0;

  // INTERNED IDS, NOT STRINGS (docs/data-layer.md §5.3, §6).
  //
  // StreamValue carries `std::string symbol`, so today a symbol is HEAP-COPIED
  // into every value on every edge of every DAG. For a chart tick that is
  // irrelevant; for a research scan over a month of raw L1 that single copy is
  // a measurable fraction of total cost. The shim's decode interns once per
  // distinct symbol (data::SymbolTable) and everything downstream carries the
  // id. Resolve back to text only at an output boundary.
  std::uint32_t symbolId    = 0;
  std::uint32_t venueId     = 0;
};

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

  // kind == Quote
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
// (docs §4.6).
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

// One book snapshot level. Snapshots are persisted on a cadence so a replay of
// an arbitrary window can seek instead of rebuilding from session open
// (docs §4.6, Q6).
struct SnapshotLevel {
  Side          side       = Side::Bid;
  std::int64_t  price      = kNoPrice;
  std::uint64_t size       = 0;
  std::uint32_t orderCount = 0;
};

} // namespace gma::data
