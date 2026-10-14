#pragma once
//
// Databento DBN -> canonical records (ENC-1482).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D6, D7, D8, D12, Q14;
//         docs/data-layer.md §2.1, §5.3, §8.3, §8.4, §8.9, C13.
//
// WHAT THIS IS
// ------------
// Bytes in, `TickRecord`/`BookRecord` out, and from those the `arrow::Schema`-
// shaped batches of `Schema.hpp`. Two DBN schemas: `trades` (rtype 0x00,
// `TradeMsg`) and `mbo` (rtype 0xA0, `MboMsg`) — the two in the first slice's
// Databento plan.
//
// IT DECODES AND MAPS. IT DOES NOT COMPUTE. No aggregation, no bucketing, no
// derived value, not one arithmetic operation on a price: ClickHouse does
// selection, gma does computation, nothing computes a value twice (SPEC D1).
// The only state it keeps is what the mapping *cannot* be done without — a
// per-order resting size, for exactly the reason `ItchAdapter` keeps one; see
// "CANCEL IS A DELTA" below.
//
// Q14 — WE DECODE DBN OURSELVES; WE DO NOT LINK DATABENTO'S CLIENT
// ---------------------------------------------------------------
// Decided here, recorded in SPEC §5/Q14 and docs/data-layer.md §15. The short
// version, honestly stated in both directions:
//
//   FOR decoding it ourselves. `databento-cpp` is a full client SDK, and the
//   90% of it we would never call is the expensive 90%: it requires OpenSSL,
//   zstd, nlohmann_json, cpp-httplib and HowardHinnant/date, and since it
//   delegated DBN decoding to the Rust `dbn` crate's C-FFI it also needs a
//   **Rust toolchain** in a C++20/CMake build that vendors everything under
//   `third_party/`. That is a far larger step than Arrow was, and D14 has just
//   dropped Parquet and DuckDB *specifically* to keep the client profile
//   small. The part we need is small and static: two packed PODs, 56 and 48
//   bytes, whose layout the vendor pins with its own `static_assert`s and
//   which is byte-identical across DBN v1, v2 and v3. And `IProviderAdapter`
//   wants a `span<const byte>` with the adapter owning reassembly — a shape
//   the vendor's stream-oriented decoder does not expose.
//
//   AGAINST. This is a wire format we do not own and must now track: a v4
//   lands on us, not on a vendor bump. Metadata and symbology are genuinely
//   fiddly (versioned `symbol_cstr_len`, a v1-only `record_count`, v3's
//   8-byte metadata alignment) and the fiddly parts are exactly where a
//   hand-rolled decoder goes wrong. Three mitigations, all of them in the
//   build rather than in a comment: the framing is **measured** against real
//   vendor bytes (`data/tests/fixtures/dbn/`, provenance there) and
//   cross-checked against `databento-cpp`'s own decoder-test expectations;
//   an unknown DBN version is **refused**, never guessed; and every declared
//   enumerator is accepted while every undeclared byte is refused, both
//   asserted per enumerator.
//
// WHAT IS DELIBERATELY NOT HERE. The historical HTTP API, the live gateway,
// the symbology *service*, zstd. `.dbn.zst` is decompressed before it reaches
// this class (and feeding it a zstd frame is refused by name rather than
// producing nonsense). ENC-1483's ingest job owns the download.
//
// PRESERVING ItchAdapter's SEMANTICS
// ---------------------------------
// This is a re-encoding of `connectors/market/src/feed/ItchAdapter.cpp`, which
// already routes the MBO vocabulary onto `OrderBookManager`'s per-order API
// with `tests/ob` and `tests/book` behind it (docs/data-layer.md §2.1). Three
// invariants carry across and are each asserted in `DatabentoAdapterTest.cpp`:
//
//  1. A PRINT NEVER CONSUMES RESTING LIQUIDITY. `ItchAdapter` sets
//     `ObTradeEvent::bookNeutral` at both of its emission sites
//     (`ItchAdapter.cpp:185`, `:317`) and `WsFeedClient.cpp:382-386` honours
//     it. DBN agrees from the other side: its own documentation for
//     `Action::Trade` and `Action::Fill` is "Does not affect the book."
//
//     So `BookAction::Trade` rows produced BY THIS ADAPTER are
//     unconditionally book-neutral, and that is why `gma.book_events` needs
//     no `book_neutral` column — for a DBN-fed table the value is a constant.
//
//     Say that precisely, because the loose version of it is false and the
//     difference decides whether a future provider may break it silently.
//     `bookNeutral` is NOT a repo-wide invariant: `FeedEvent.hpp:64` defaults
//     it to `false`, four test sites in `tests/feed/FeedEventDispatchTest.cpp`
//     construct an `ObTradeEvent` without setting it, and
//     `WsFeedClient.cpp:385-387` has a live `!e.bookNeutral` branch that DOES
//     call `obManager_->onTrade`. A non-neutral trade is representable and is
//     what a feed whose print *is* the book mutation would legitimately use.
//     What holds is narrower and sufficient: both of `ItchAdapter`'s
//     production emission sites set it, and DBN's contract makes it true of
//     every row this adapter emits. A provider that is not DBN must state its
//     own answer rather than inherit this one. `TickRecord::bookNeutral` is
//     set on every `trades` row on the same basis (SPEC D12, docs C13).
//  2. `epoch` IS BUMPED BY A CLEAR, per instrument, and every record after one
//     carries the new epoch. The Clear row itself carries the NEW epoch,
//     matching `OrderBookManager::onReset(symbol, newEpoch)`.
//
//     WHAT ACTUALLY STOPS A PRE-RESET ORDER BEING MATCHED BY A POST-RESET
//     MESSAGE, here, is the erase loop in the `Clear` case: it drops every
//     tracked order for that instrument, so a later Cancel for a pre-reset
//     `order_id` finds nothing and resolves as an unknown-order Delete rather
//     than against a stale resting size. (`gma::OrderKey` in
//     `connectors/market/include/gma/book/OrderBook.hpp` carries an epoch and
//     is what protects the BOOK downstream; this adapter's own private
//     `OrderKey` is `{instrumentId, orderId}` and carries no epoch — it does
//     not need one, because the map is emptied instead.) Removing that erase
//     loop is caught by `ClearBumpsTheEpochAndDropsTrackedOrders`.
//  3. CANCEL IS A DELTA, so it needs the resting size. DBN's `Cancel` `size`
//     is the quantity *removed*, exactly like ITCH's partial cancel — which
//     is why `ItchAdapter` keeps `orders_` and resolves cancel-of-N into
//     `ObUpdateEvent(remaining)` or `ObDeleteEvent` (`ItchAdapter.cpp:213-228`).
//     `BookRecord::size` means one thing everywhere (the order's absolute
//     size, as `OrderBookManager::onUpdate` wants it); making it a delta for
//     one action would be a type pun nothing downstream could detect. So this
//     adapter keeps the same bounded order map, with the same eviction, and
//     the same `removed >= remaining -> Delete` guard.
//
// provider_seq COMES FROM DATABENTO AND IS NEVER SYNTHESIZED (SPEC D6).
// `RecordHeader::providerSeq = MboMsg::sequence` / `TradeMsg::sequence`, the
// venue sequence, which is stable across a re-download — which is what makes
// re-ingest converge to the same rows and the splice's dedup exact. Note that
// `ItchAdapter` stamps an *optional* `seq` only when the JSON carries one
// (`doc.HasMember("seq")`), i.e. the ITCH path is seq-less today; DBN supplies
// one, so here it is not optional and `Stats::recordsIn` is the only place a
// missing one could hide (it cannot — the field is fixed-width).
//
// PRICES ARE AN EXACT INTEGER PATH (SPEC D8). DBN prices are `int64` at 1e-9
// and `kPriceScale` is 1e-9, so the mapping is an assignment. There is no
// `double` anywhere in this header or its .cpp's price path, and `toFixed()` is
// never called. The one translation is the sentinel: DBN's "no price" is
// `kUndefPrice` (INT64_MAX) and ours is `kNoPrice` (INT64_MIN).
//

#include "gma/data/Record.hpp"
#include "gma/data/Schema.hpp"
#include "gma/data/SymbolTable.hpp"

#include <arrow/result.h>
#include <arrow/status.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace arrow { class MemoryPool; class RecordBatch; }

namespace gma::data {

// ===========================================================================
// dbn — the wire vocabulary.
//
// MEASURED, NOT REMEMBERED. Every constant and every offset below was read out
// of the committed fixtures in `data/tests/fixtures/dbn/` and cross-checked
// against `databento-cpp`'s own decoder tests; `PROVENANCE.md` beside the
// fixtures records both. docs/data-layer.md §8.9 listed "the exact DBN field
// names" as unverified and from memory — this namespace is the answer, and the
// names here are the vendor's own spellings.
// ===========================================================================
namespace dbn {

// ---- framing -------------------------------------------------------------

inline constexpr std::array<char, 3> kMagic = {'D', 'B', 'N'};
// "DBN" + a one-byte version. Version 0 denotes a legacy DBZ file and is
// refused rather than read.
inline constexpr std::size_t kPrefixLen        = 4;
inline constexpr std::size_t kMetadataLenLen   = 4;   // u32, little-endian
inline constexpr std::uint8_t kMinVersion      = 1;
inline constexpr std::uint8_t kMaxVersion      = 3;

// A zstd frame, so a `.dbn.zst` fed here is named rather than mis-parsed.
inline constexpr std::array<std::uint8_t, 4> kZstdMagic = {0x28, 0xB5, 0x2F, 0xFD};

// Fixed metadata block, after the 8-byte prefix. 100 bytes in every version;
// what fills it differs (v1 carries a `record_count` the others spend on
// `symbol_cstr_len` plus six more reserved bytes).
inline constexpr std::size_t kFixedMetadataLen = 100;
inline constexpr std::size_t kDatasetCstrLen   = 16;
inline constexpr std::size_t kReservedLen      = 53;  // v2+
inline constexpr std::size_t kReservedLenV1    = 47;
inline constexpr std::size_t kSymbolCstrLenV1  = 22;  // not in v1 metadata
// v3 rounds the metadata length up to a multiple of 8 with zero bytes.
inline constexpr std::size_t kMetadataAlignment    = 8;
inline constexpr std::uint8_t kFirstAlignedVersion = 3;

// Record framing. `RecordHeader::length` counts 32-bit words, header included.
inline constexpr std::size_t kRecordHeaderLen   = 16;
inline constexpr std::size_t kLengthMultiplier  = 4;
inline constexpr std::size_t kMboMsgLen         = 56;
inline constexpr std::size_t kTradeMsgLen       = 48;

// ---- sentinels (vendor spellings) ---------------------------------------

inline constexpr std::int64_t  kUndefPrice     = std::numeric_limits<std::int64_t>::max();
inline constexpr std::uint32_t kUndefOrderSize = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint64_t kUndefTimestamp = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint64_t kNullLimit       = 0;
inline constexpr std::uint64_t kNullRecordCount = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint16_t kNullSchema      = std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint8_t  kNullSType       = std::numeric_limits<std::uint8_t>::max();

// ---- enumerations -------------------------------------------------------
//
// The DECLARED domain of each. A byte inside it is accepted; a byte outside it
// is refused. Both halves are asserted per enumerator, because a suite that
// only tests rejection cannot catch a bound that over-rejects a legal value —
// the bug ENC-1481 shipped and fixed.

enum class RType : std::uint8_t {
  Mbp0            = 0x00,  // the `trades` schema      -> TradeMsg  (CONSUMED)
  Mbp1            = 0x01,
  Mbp10           = 0x0A,
  OhlcvDeprecated = 0x11,
  Status          = 0x12,
  InstrumentDef   = 0x13,
  Imbalance       = 0x14,
  Error           = 0x15,
  SymbolMapping   = 0x16,
  System          = 0x17,
  Statistics      = 0x18,
  Ohlcv1S         = 0x20,
  Ohlcv1M         = 0x21,
  Ohlcv1H         = 0x22,
  Ohlcv1D         = 0x23,
  OhlcvEod        = 0x24,
  Mbo             = 0xA0,  // the `mbo` schema         -> MboMsg    (CONSUMED)
  Cmbp1           = 0xB1,
  Cbbo1S          = 0xC0,
  Cbbo1M          = 0xC1,
  Tcbbo           = 0xC2,
  Bbo1S           = 0xC3,
  Bbo1M           = 0xC4,
};

enum class Schema : std::uint16_t {
  Mbo = 0, Mbp1 = 1, Mbp10 = 2, Tbbo = 3, Trades = 4,
  Ohlcv1S = 5, Ohlcv1M = 6, Ohlcv1H = 7, Ohlcv1D = 8,
  Definition = 9, Statistics = 10, Status = 11, Imbalance = 12, OhlcvEod = 13,
  Cmbp1 = 14, Cbbo1S = 15, Cbbo1M = 16, Tcbbo = 17, Bbo1S = 18, Bbo1M = 19,
};

enum class SType : std::uint8_t {
  InstrumentId = 0, RawSymbol = 1, Smart = 2, Continuous = 3, Parent = 4,
  NasdaqSymbol = 5, CmsSymbol = 6, Isin = 7, UsCode = 8, BbgCompId = 9,
  BbgCompTicker = 10, Figi = 11, FigiTicker = 12, ListingId = 13,
  IssuerId = 14, SecurityId = 15,
};

// An order event or order-book operation. The vendor's doc comment for each is
// quoted, because the mapping below turns on precisely those words.
enum class Action : char {
  Modify = 'M',  // "An existing order was modified: price and/or size."
  Trade  = 'T',  // "An aggressing order traded. Does not affect the book."
  Fill   = 'F',  // "An existing order was filled. Does not affect the book."
  Cancel = 'C',  // "An order was fully or partially cancelled."
  Add    = 'A',  // "A new order was added to the book."
  Clear  = 'R',  // "Reset the book; clear all orders for an instrument."
  None   = 'N',  // "Has no effect on the book, but may carry flags [...]"
};

// "The side of the market for resting orders, or the side of the aggressor for
// trades." THREE-VALUED: `gma::data::Side` has only Bid and Ask, so `None` has
// no representation there — see `kGmaFlagSideUnspecified`.
enum class Side : char {
  Ask  = 'A',  // "A sell order or sell aggressor in a trade."
  Bid  = 'B',  // "A buy order or a buy aggressor in a trade."
  None = 'N',  // "No side specified by the original source."
};

inline constexpr std::array<Action, 7> kAllActions = {
    Action::Modify, Action::Trade, Action::Fill, Action::Cancel,
    Action::Add,    Action::Clear, Action::None};
inline constexpr std::array<Side, 3> kAllSides = {Side::Ask, Side::Bid, Side::None};
inline constexpr std::array<RType, 23> kAllRTypes = {
    RType::Mbp0,  RType::Mbp1,          RType::Mbp10,   RType::OhlcvDeprecated,
    RType::Status, RType::InstrumentDef, RType::Imbalance, RType::Error,
    RType::SymbolMapping, RType::System, RType::Statistics, RType::Ohlcv1S,
    RType::Ohlcv1M, RType::Ohlcv1H,     RType::Ohlcv1D,  RType::OhlcvEod,
    RType::Mbo,    RType::Cmbp1,        RType::Cbbo1S,   RType::Cbbo1M,
    RType::Tcbbo,  RType::Bbo1S,        RType::Bbo1M};

bool isDeclaredAction(char c) noexcept;
bool isDeclaredSide(char c) noexcept;
bool isDeclaredRType(std::uint8_t r) noexcept;
// Human-readable, for refusal messages. "?" for anything undeclared.
const char* actionName(char c) noexcept;
const char* sideName(char c) noexcept;

// ---- provider condition flags (a single byte on the wire) ---------------

inline constexpr std::uint8_t kFlagLast              = 1u << 7;
inline constexpr std::uint8_t kFlagTob               = 1u << 6;
inline constexpr std::uint8_t kFlagSnapshot          = 1u << 5;
inline constexpr std::uint8_t kFlagMbp               = 1u << 4;
inline constexpr std::uint8_t kFlagBadTsRecv         = 1u << 3;
inline constexpr std::uint8_t kFlagMaybeBadBook      = 1u << 2;
inline constexpr std::uint8_t kFlagPublisherSpecific = 1u << 1;

// ---- records ------------------------------------------------------------
//
// HOST-ORDER, DECODED STRUCTS — NOT A VIEW OF THE WIRE. These are filled field
// by field with explicit little-endian loads. `reinterpret_cast`ing the wire
// into a struct would be wrong three ways at once (alignment of an unaligned
// byte span, strict aliasing, and a big-endian host) and would make the
// layout an accident of the compiler rather than a documented fact.

struct RecordHeader {
  std::uint8_t  length       = 0;  // in 32-bit words, header included
  std::uint8_t  rtype        = 0;
  std::uint16_t publisherId  = 0;
  std::uint32_t instrumentId = 0;
  std::uint64_t tsEvent      = 0;
};

// rtype 0xA0. 56 bytes: hd(16) orderId(8) price(8) size(4) flags(1)
// channelId(1) action(1) side(1) tsRecv(8) tsInDelta(4) sequence(4).
struct MboMsg {
  RecordHeader  hd;
  std::uint64_t orderId   = 0;
  std::int64_t  price     = kUndefPrice;
  std::uint32_t size      = 0;
  std::uint8_t  flags     = 0;
  std::uint8_t  channelId = 0;
  char          action    = 'N';
  char          side      = 'N';
  std::uint64_t tsRecv    = 0;
  std::int32_t  tsInDelta = 0;
  std::uint32_t sequence  = 0;
};

// rtype 0x00. 48 bytes: hd(16) price(8) size(4) action(1) side(1) flags(1)
// depth(1) tsRecv(8) tsInDelta(4) sequence(4). NOTE the field ORDER differs
// from MboMsg — action/side come before flags here and after it there.
struct TradeMsg {
  RecordHeader  hd;
  std::int64_t  price     = kUndefPrice;
  std::uint32_t size      = 0;
  char          action    = 'T';
  char          side      = 'N';
  std::uint8_t  flags     = 0;
  std::uint8_t  depth     = 0;
  std::uint64_t tsRecv    = 0;
  std::int32_t  tsInDelta = 0;
  std::uint32_t sequence  = 0;
};

// ---- metadata -----------------------------------------------------------

struct MappingInterval {
  std::uint32_t startDate = 0;  // ISO-8601 as yyyymmdd, e.g. 20201228
  std::uint32_t endDate   = 0;  // HALF-OPEN: [startDate, endDate)
  std::string   symbol;
};

struct SymbolMapping {
  std::string                  rawSymbol;
  std::vector<MappingInterval> intervals;
};

struct Metadata {
  std::uint8_t  version       = 0;
  std::string   dataset;
  // nullopt when the file mixes schemas (`kNullSchema` on the wire).
  std::optional<std::uint16_t> schema;
  std::uint64_t start         = 0;
  std::uint64_t end           = 0;   // kUndefTimestamp == open-ended
  std::uint64_t limit         = 0;   // 0 == no limit
  std::optional<std::uint64_t> recordCount;  // v1 only, and often null there
  std::optional<std::uint8_t>  stypeIn;
  std::uint8_t  stypeOut      = 0;
  bool          tsOut         = false;
  std::uint16_t symbolCstrLen = 0;
  std::vector<std::string>   symbols;
  std::vector<std::string>   partial;
  std::vector<std::string>   notFound;
  std::vector<SymbolMapping> mappings;
  // Bytes of zero padding the metadata block ended with (v3's 8-byte
  // alignment). Surfaced rather than swallowed so the fixtures can assert it:
  // 0 for v1/v2, 7 for the committed v3 file.
  std::size_t   endPadding    = 0;
};

}  // namespace dbn

// ===========================================================================
// gma-side flag bits
// ===========================================================================
//
// `TickRecord::flags` and `BookRecord::flags` are `uint16` and DBN's own flags
// are a single byte, so 0xFF00 is headroom rather than an invention. One bit is
// used, and it exists because of a REAL GAP in ENC-1481's schema rather than
// for convenience:
//
//   `BookRecord::side` is `data::Side{Bid, Ask}` and `gma.book_events.side` is
//   its UInt8. DBN's `Side` is three-valued, and `Side::None` is normal on a
//   `Trade` record from a venue that does not report an aggressor. There is
//   nowhere in `BookRecord` to put "no side" — `TickRecord` is fine because
//   `Aggressor::Unknown` exists, but `BookRecord` has no aggressor field.
//
// So a `Trade`/`Fill` row whose DBN side was 'N' carries this bit and leaves
// `side` at its default. The recommended fix is upstream of this file and is
// reported with the ticket: `BookRecord` should carry an `Aggressor` for
// `BookAction::Trade` rows, or `side` should mirror DBN's three values. Until
// then this bit is the lossless answer and is asserted in the tests.
inline constexpr std::uint16_t kGmaFlagSideUnspecified = 0x0100;

// ===========================================================================
// DatabentoAdapter
// ===========================================================================

class DatabentoAdapter {
 public:
  struct Config {
    // Normalizer session. The dedup key is the PAIR (sessionId, providerSeq).
    std::uint32_t sessionId = 0;

    // Venue label interned into `venueId` on every row. Empty means "use the
    // DBN metadata's `dataset`" (e.g. "GLBX.MDP3"), which is what the ingest
    // job wants and is checkable by hand in ClickHouse.
    //
    // DBN's `publisher_id` is the finer-grained answer (it denotes dataset AND
    // venue), but resolving it needs the vendor's 60 KB publisher table and
    // `RecordHeader` has no field to retain it in. Deliberately not vendored
    // at one dataset; the adapter instead REFUSES a stream whose publisher_id
    // changes mid-decode, because that is the only case where a single venue
    // label would be wrong.
    std::string venue;

    // Ticker for every row, bypassing symbology. Empty means "resolve from the
    // metadata's mappings" (SPEC D7). A non-empty value is for a file whose
    // mappings do not mention `instrument_id` in either direction; it is NOT a
    // convenience default, because a wrong ticker in L1 is undetectable later.
    std::string symbolOverride;

    // Bound on the resting-order map, with the same justification as
    // `ItchAdapter::kMaxTrackedOrders`: entries are erased on
    // delete/full-cancel/clear, but a stream of orders never subsequently
    // removed would grow it without limit. At the cap AN ARBITRARY entry is
    // evicted, which forfeits delta resolution for that one order and keeps
    // the map at or below the cap.
    //
    // "Arbitrary", not "oldest": the eviction is `orders_.erase(begin())` and
    // `std::unordered_map::begin()` is whatever bucket order puts first, which
    // the container does not relate to insertion order. `ItchAdapter`'s own
    // comment at the equivalent site calls it "the oldest bucket entry" and
    // that is not a promise the container makes. Nothing here depends on
    // which entry goes — a stream that hits this cap has already lost delta
    // resolution somewhere — but the comment should not claim an ordering the
    // code cannot keep.
    std::size_t maxTrackedOrders = 1u << 20;

    // THE ONE POLICY SWITCH, AND IT IS A MEASUREMENT HOOK, NOT A PREFERENCE.
    //
    // DBN says a `Fill` "Does not affect the book" — i.e. the resting order's
    // reduction arrives separately as a `Cancel` or `Modify`. `ItchAdapter`
    // does the opposite: ITCH has no such separate message, so
    // `routeOrderExecuted` applies the reduction itself (delete on a full
    // fill, update on a partial) AND emits the book-neutral print.
    //
    // Default `false` = trust DBN's own contract: a Fill yields the print
    // only. Applying both the Fill and a following Cancel would DOUBLE-REDUCE
    // the order; applying neither leaves phantom size. Which it is cannot be
    // measured from the public sample fixtures (they contain only Cancels), so
    // it is an open fact for ENC-1483/T5 against real MBO, and this flag is
    // where the answer lands. Both branches are tested.
    bool fillReducesRestingOrder = false;
  };

  struct Stats {
    std::uint64_t bytesIn        = 0;
    std::uint64_t recordsIn      = 0;   // DBN records framed
    std::uint64_t recordsSkipped = 0;   // declared rtype, not a schema we consume
    std::uint64_t ticksOut       = 0;   // TickRecords produced
    std::uint64_t bookEventsOut  = 0;   // BookRecords produced
    std::uint64_t actionNone     = 0;   // Action::None — accepted, no book effect
    std::uint64_t clears         = 0;   // Action::Clear, i.e. epoch bumps
    std::uint64_t unknownOrder   = 0;   // Cancel/Modify/Fill for an unseen order_id
    std::uint64_t evictedOrders  = 0;   // maxTrackedOrders pressure
    std::uint64_t sideUnspecified= 0;   // DBN side 'N' on a book row
    std::uint64_t undefPrice     = 0;   // kUndefPrice -> kNoPrice
    std::uint64_t undefSize      = 0;   // kUndefOrderSize seen
  };

  // `symbols` and `venues` are the tables the batches are encoded against and
  // outlive the adapter. Taken by reference, like `Schema.hpp`'s codec, so
  // there is exactly one interning table per process.
  DatabentoAdapter(Config cfg, SymbolTable& symbols, SymbolTable& venues);

  // ---- feeding ----------------------------------------------------------

  // Push bytes. A DBN record may be split at ANY byte across calls, so the
  // adapter buffers the tail and resumes. Decoded records are appended to the
  // `trades()` / `bookEvents()` vectors.
  //
  // On a refusal the adapter is POISONED: this and every later `feed` return
  // the same Status and nothing further is appended. A decoder that kept going
  // after a framing error would be resynchronising on data it has already
  // proven it cannot parse.
  arrow::Status feed(std::span<const std::byte> raw);

  // Bytes held back because the frame is incomplete.
  std::size_t pendingBytes() const noexcept { return buf_.size() - cursor_; }

  // End of stream. Fails when bytes are still held: a partial record at the
  // end of a file or at a disconnect IS A GAP and must be reported, never
  // held until more bytes silently complete it from a different session.
  arrow::Status finish();

  // ---- results ----------------------------------------------------------

  const std::optional<dbn::Metadata>& metadata() const noexcept { return meta_; }
  const Stats& stats() const noexcept { return stats_; }

  // The gateway diagnostics DBN carries that no canonical column holds.
  // SURFACED RATHER THAN DECODED AND DROPPED, for two reasons.
  //
  // `channel_id` is the scope within which DBN's `sequence` — which becomes
  // our `provider_seq` — is monotonic. D6's dedup key is
  // `(session_id, provider_seq)` and `book_events`' sort key ends in it, so a
  // stream that mixes channels is a stream where that is not a per-symbol
  // total order. `IProviderAdapter::ProviderCapabilities::seqIsGlobal` exists
  // for exactly this distinction, and ENC-1483/T6 has to check it against a
  // real 7-day file; `minChannelId != maxChannelId` is the two-byte answer.
  //
  // And a field that is decoded but unreadable is a field no test can gate,
  // which is how a wrong byte offset survives a green suite. `ts_in_delta` is
  // the only signed-32 field in either record, so this accessor is what makes
  // that load path reachable at all.
  struct Gateway {
    std::uint8_t channelId    = 0;      // most recent record (MboMsg only)
    std::int32_t tsInDelta    = 0;      // most recent record: ts_recv - ts_event, ns
    std::uint8_t minChannelId = 0xFF;   // over the whole stream
    std::uint8_t maxChannelId = 0;
  };
  const Gateway& gateway() const noexcept { return gw_; }

  const std::vector<TickRecord>& trades() const noexcept { return ticks_; }
  const std::vector<BookRecord>& bookEvents() const noexcept { return book_; }

  std::vector<TickRecord> takeTrades();
  std::vector<BookRecord> takeBookEvents();

  // The canonical batches — `Schema.hpp`'s encoders over what has accumulated.
  // These are the ticket's deliverable; the POD vectors above are the input to
  // them and are exposed so a test can assert the mapping without going
  // through Arrow.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> tradesBatch(
      arrow::MemoryPool* pool = nullptr) const;
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> bookEventsBatch(
      arrow::MemoryPool* pool = nullptr) const;

  // Epoch currently in force for an instrument (0 until its first Clear).
  std::uint32_t epochOf(std::uint32_t instrumentId) const;
  // Resting size this adapter believes an order holds, or nullopt if untracked.
  std::optional<std::uint64_t> restingSize(std::uint32_t instrumentId,
                                           std::uint64_t orderId) const;
  // How many orders are tracked right now. Exists so a test can assert the
  // INVARIANT (`<= Config::maxTrackedOrders`) rather than the eviction
  // counter: a no-op eviction that still bumps `Stats::evictedOrders` passes
  // a counter check and fails this one.
  std::size_t trackedOrders() const noexcept { return orders_.size(); }

 private:
  struct OrderKey {
    std::uint32_t instrumentId;
    std::uint64_t orderId;
    bool operator==(const OrderKey&) const noexcept = default;
  };
  struct OrderKeyHash {
    std::size_t operator()(const OrderKey& k) const noexcept {
      return std::hash<std::uint64_t>{}(k.orderId ^ (std::uint64_t{k.instrumentId} << 40));
    }
  };

  arrow::Status drain();
  arrow::Status parseMetadata(std::span<const std::byte> block, std::uint8_t version);
  arrow::Status onMbo(const dbn::MboMsg& m);
  arrow::Status onTrade(const dbn::TradeMsg& m);
  arrow::Result<RecordHeader> makeHeader(const dbn::RecordHeader& hd,
                                         std::uint64_t tsRecv,
                                         std::uint32_t sequence);
  arrow::Result<SymbolId> resolveSymbol(std::uint32_t instrumentId,
                                        std::uint64_t tsEventNs);
  void trackOrder(const OrderKey& k, std::uint64_t size);

  Config       cfg_;
  SymbolTable& symbols_;
  SymbolTable& venues_;

  // One resolved symbology window. The ticker is interned ONCE, at
  // metadata-parse time, and not per record: `SymbolTable::intern` takes a
  // write lock on a `shared_mutex`, and taking it per record on an MBO stream
  // would put a lock on the hot path — the exact thing SymbolTable.hpp says
  // the ids exist to avoid.
  struct SymbolWindow {
    std::uint32_t startDate = 0;  // yyyymmdd, HALF-OPEN [startDate, endDate)
    std::uint32_t endDate   = 0;
    SymbolId      id        = kInvalidSymbolId;
  };

  std::optional<dbn::Metadata> meta_;
  std::optional<std::uint16_t> publisherId_;   // asserted stable across the stream
  SymbolId                     venueId_ = kInvalidSymbolId;
  SymbolId                     overrideSymbolId_ = kInvalidSymbolId;
  // instrument_id -> date-ranged tickers, built from the metadata mappings in
  // whichever direction they were recorded.
  std::unordered_map<std::uint32_t, std::vector<SymbolWindow>> symbology_;

  std::vector<std::byte> buf_;
  std::size_t            cursor_ = 0;
  bool                   haveMeta_ = false;
  arrow::Status          poison_;

  std::unordered_map<OrderKey, std::uint64_t, OrderKeyHash> orders_;
  std::unordered_map<std::uint32_t, std::uint32_t>          epochs_;
  Gateway                                                   gw_;

  std::vector<TickRecord> ticks_;
  std::vector<BookRecord> book_;
  Stats                   stats_;
};

}  // namespace gma::data
