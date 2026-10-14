#include "gma/data/Schema.hpp"

#include <arrow/array/array_dict.h>
#include <arrow/array/array_primitive.h>
#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/util/key_value_metadata.h>
#include <arrow/memory_pool.h>

#include <charconv>
#include <string>
#include <unordered_map>

namespace gma::data {
namespace {

// ---------------------------------------------------------------------------
// Types used by more than one table, built once.
// ---------------------------------------------------------------------------

std::shared_ptr<arrow::DataType> nanosUtc() {
  return arrow::timestamp(arrow::TimeUnit::NANO, "UTC");
}

std::shared_ptr<arrow::DataType> symbolType() {
  return arrow::dictionary(arrow::int32(), arrow::utf8());
}

std::shared_ptr<arrow::DataType> venueType() {
  // int16 indices, not int32: a venue space has a handful of entries, and at
  // MBO row counts two bytes a row is not nothing (docs/data-layer.md §8.3).
  return arrow::dictionary(arrow::int16(), arrow::utf8());
}

// Every field below is non-nullable — see the note in Schema.hpp.
arrow::FieldVector commonFields() {
  return {
      arrow::field("ts_event", nanosUtc(), /*nullable=*/false),
      arrow::field("ts_recv", nanosUtc(), false),
      arrow::field("provider_seq", arrow::uint64(), false),
      arrow::field("session_id", arrow::uint32(), false),
      arrow::field("symbol", symbolType(), false),
      arrow::field("instrument_id", arrow::uint32(), false),
      arrow::field("venue", venueType(), false),
  };
}

std::shared_ptr<arrow::KeyValueMetadata> schemaMetadata() {
  return arrow::key_value_metadata({std::string(kSchemaVersionKey)},
                                   {std::to_string(kSchemaVersion)});
}

std::shared_ptr<arrow::Schema> makeSchema(arrow::FieldVector payload) {
  auto fields = commonFields();
  fields.insert(fields.end(), payload.begin(), payload.end());
  return arrow::schema(std::move(fields), schemaMetadata());
}

// ---------------------------------------------------------------------------
// Dictionary column helpers
// ---------------------------------------------------------------------------
//
// Arrow ships Dictionary32Builder<StringType> but nothing for int16 indices,
// so both columns go through one hand-rolled builder instead of one column
// using a library builder and the other not. Two mechanisms for one job is how
// the two columns drift apart.
//
// The dictionary is built in order of first appearance, which makes the output
// deterministic for a given input order — load-bearing for a round-trip test
// and for anything that ever diffs two encodings of the same batch.

template <typename IndexBuilder>
class DictColumn {
 public:
  DictColumn(std::shared_ptr<arrow::DataType> type, const SymbolTable& table,
             arrow::MemoryPool* pool, const char* what)
      : type_(std::move(type)), table_(table), what_(what),
        indices_(pool), values_(pool) {}

  arrow::Status append(SymbolId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
      if (!table_.contains(id)) {
        // An unknown id must not become "". A blank ticker in L1 is
        // indistinguishable from a provider gap, which is exactly the
        // confusion SPEC D11 exists to prevent one level up.
        return arrow::Status::Invalid(
            "gma::data: ", what_, " id ", id,
            " is not interned in the table supplied to the encoder");
      }
      ARROW_RETURN_NOT_OK(values_.Append(table_.name(id)));
      const auto next = static_cast<typename IndexBuilder::value_type>(index_.size());
      it = index_.emplace(id, next).first;
    }
    return indices_.Append(it->second);
  }

  arrow::Result<std::shared_ptr<arrow::Array>> finish() {
    std::shared_ptr<arrow::Array> indices;
    ARROW_RETURN_NOT_OK(indices_.Finish(&indices));
    std::shared_ptr<arrow::Array> values;
    ARROW_RETURN_NOT_OK(values_.Finish(&values));
    return arrow::DictionaryArray::FromArrays(type_, indices, values);
  }

 private:
  std::shared_ptr<arrow::DataType> type_;
  const SymbolTable& table_;
  const char* what_;
  IndexBuilder indices_;
  arrow::StringBuilder values_;
  std::unordered_map<SymbolId, typename IndexBuilder::value_type> index_;
};

using SymbolColumn = DictColumn<arrow::Int32Builder>;
using VenueColumn  = DictColumn<arrow::Int16Builder>;

// Interns a dictionary column's values ONCE PER DISTINCT VALUE and returns the
// per-dictionary-index id map. This is the line docs/data-layer.md §5.3 is
// about: the per-row cost of a symbol becomes an array index, not a heap copy.
arrow::Result<std::vector<SymbolId>> internDictionary(
    const arrow::Array& column, SymbolTable& table, const char* what) {
  if (column.type_id() != arrow::Type::DICTIONARY) {
    return arrow::Status::Invalid("gma::data: ", what,
                                  " column is not a dictionary array");
  }
  const auto& dict = static_cast<const arrow::DictionaryArray&>(column);
  const auto& values = *dict.dictionary();
  if (values.type_id() != arrow::Type::STRING) {
    return arrow::Status::Invalid("gma::data: ", what,
                                  " dictionary values are not utf8");
  }
  const auto& strings = static_cast<const arrow::StringArray&>(values);
  // A NULL HERE IS INVISIBLE TO Array::null_count() ON THE COLUMN, which
  // reports the null count of the dictionary's INDICES, not of its values. An
  // unchecked null value becomes GetView(i) == "" and then an ordinary interned
  // id (SymbolTableTest.EmptyStringGetsAnOrdinaryId pins that), i.e. a record
  // naming a real symbol called "" with Status::OK() — the same confusion the
  // encoder refuses an un-interned id to avoid, arriving from the other end.
  if (strings.null_count() != 0) {
    return arrow::Status::Invalid(
        "gma::data: ", what, " dictionary holds ", strings.null_count(),
        " null value(s); a null ticker would decode to an interned \"\", which "
        "is indistinguishable from a provider gap");
  }
  if (dict.indices()->null_count() != 0) {
    return arrow::Status::Invalid(
        "gma::data: ", what, " dictionary indices hold ",
        dict.indices()->null_count(),
        " null(s), but the column is declared non-nullable");
  }
  std::vector<SymbolId> ids;
  ids.reserve(static_cast<std::size_t>(strings.length()));
  for (std::int64_t i = 0; i < strings.length(); ++i) {
    ids.push_back(table.intern(strings.GetView(i)));
  }
  return ids;
}

// Reads a dictionary column's index at row i and maps it through `ids`.
arrow::Result<SymbolId> dictIdAt(const arrow::Array& column,
                                 const std::vector<SymbolId>& ids,
                                 std::int64_t row, const char* what) {
  const auto& dict = static_cast<const arrow::DictionaryArray&>(column);
  const auto& idx = *dict.indices();
  std::int64_t slot = -1;
  switch (idx.type_id()) {
    case arrow::Type::INT16:
      slot = static_cast<const arrow::Int16Array&>(idx).Value(row);
      break;
    case arrow::Type::INT32:
      slot = static_cast<const arrow::Int32Array&>(idx).Value(row);
      break;
    default:
      return arrow::Status::Invalid("gma::data: ", what,
                                    " dictionary indices are neither int16 nor int32");
  }
  if (slot < 0 || static_cast<std::size_t>(slot) >= ids.size()) {
    return arrow::Status::Invalid("gma::data: ", what,
                                  " dictionary index out of range");
  }
  return ids[static_cast<std::size_t>(slot)];
}

// ---------------------------------------------------------------------------
// Typed column fetch
// ---------------------------------------------------------------------------
//
// By NAME and not by position, and type-checked, so a schema edit that
// reorders or retypes a column produces a named error here rather than a
// plausible wrong number downstream. Done once per column per batch, so the
// string compare is not on any hot path.

template <typename ArrayType>
arrow::Result<const ArrayType*> column(const arrow::RecordBatch& batch,
                                       const char* name) {
  auto col = batch.GetColumnByName(name);
  if (col == nullptr) {
    return arrow::Status::Invalid("gma::data: batch has no column '", name, "'");
  }
  const auto* typed = dynamic_cast<const ArrayType*>(col.get());
  if (typed == nullptr) {
    return arrow::Status::Invalid("gma::data: column '", name, "' has type ",
                                  col->type()->ToString(),
                                  ", which is not what the canonical schema declares");
  }
  return typed;
}

// ---------------------------------------------------------------------------
// Enum domain checks
// ---------------------------------------------------------------------------
//
// The column TYPE for `action`, `side` and `aggressor` is uint8, which is
// legal for every one of the 256 values a byte can hold — so the schema check
// above cannot catch an out-of-domain one, and `static_cast<Side>(7)` is a
// `Side` matching no enumerator. That value then reaches §4.6's dispatch onto
// OrderBookManager::onAdd/onUpdate/onDelete/onReset, hits no case, and the
// event is SILENTLY DROPPED while the decode reported OK. A batch arriving
// over `FORMAT ArrowStream` (ENC-1485's transport) is not under our control,
// so this is a wire input, not an internal invariant.
//
// THE BOUNDS ARE NAMED IN TERMS OF THE LAST ENUMERATOR, not as magic numbers,
// so a renamed enumerator is a compile error rather than a silent off-by-one.
// What they cannot be is self-maintaining: C++ gives no way to ask for an
// enum's largest enumerator, and these three enums mirror the engine's own
// (Record.hpp's header comment) one-for-one, so adding a sentinel to them is
// not available either.
//
// SO BE PRECISE ABOUT WHAT IS GUARDED, because the first version of this
// comment claimed a drift guard that does not exist. ADDING AN ENUMERATOR
// PAST THE ONE NAMED BELOW REQUIRES EDITING THIS LINE BY HAND. If you forget,
// the codec rejects a value that is now legal, every existing test stays
// green, and `DecodeAcceptsEveryDeclaredEnumerator` in
// data/tests/SchemaRoundTripTest.cpp is the one place that will tell you —
// but only once you add the new enumerator to ITS list too, which is why that
// test enumerates them one per line instead of looping a range. The tests DO
// catch the opposite drift (a bound raised here without the domain tests
// following), and they catch any out-of-domain wire value, which is what the
// check is actually for.
inline constexpr std::uint8_t kMaxSideValue =
    static_cast<std::uint8_t>(Side::Ask);
inline constexpr std::uint8_t kMaxAggressorValue =
    static_cast<std::uint8_t>(Aggressor::Sell);
inline constexpr std::uint8_t kMaxBookActionValue =
    static_cast<std::uint8_t>(BookAction::Clear);

template <typename Enum>
arrow::Result<Enum> enumAt(const arrow::UInt8Array& column, std::int64_t row,
                           std::uint8_t maxValue, const char* what) {
  const std::uint8_t raw = column.Value(row);
  if (raw > maxValue) {
    return arrow::Status::Invalid(
        "gma::data: row ", row, " column '", what, "' holds ",
        static_cast<unsigned>(raw), ", which is outside the declared domain "
        "0..", static_cast<unsigned>(maxValue),
        " — refusing to produce a record whose enum matches no enumerator "
        "(it would be dropped silently by the dispatch downstream)");
  }
  return static_cast<Enum>(raw);
}

// Refuses a batch whose schema is not the one asked for. Metadata is excluded
// from the comparison on purpose: the version lives there and is checked
// separately, with a message that says so.
//
// It also refuses a batch that is merely INTERNALLY INCONSISTENT, which is
// where the out-of-bounds reads were. `arrow::RecordBatch::Make` does not
// validate, and the IPC reader does not run ValidateFull, so a batch whose
// num_rows exceeds a column's length is constructible — and every decoder
// below trusts num_rows as its loop bound, which turns a truncated or corrupt
// ArrowStream into `price->Value(i)` reading past the Arrow buffer. That is an
// out-of-bounds read, not a Status, so it is checked before the first Value()
// call rather than after. Validate() is O(columns), not O(rows).
//
// WHAT Validate() DOES NOT COVER, stated because "checked before the first
// read" is true of Value() and not of everything: it is
// ValidateBatch(full_validation=false), which checks each column's length and
// type against the schema but does NOT validate offset *values* inside a
// variable-width array (arrow/array/validate.cc gates that on full
// validation). So a dictionary whose StringArray offsets are corrupt still
// reaches GetView() in internDictionary. ValidateFull() would catch it and is
// deliberately not called: it is O(rows) on every batch on the decode path,
// which is the one place in this design that is explicitly a per-batch cost.
// A corrupt-offsets ArrowStream is therefore still out of scope here; the
// transport, not the codec, is where that gets a checksum (ENC-1485).
//
// Nulls are refused for the same reason one level up: every field in every
// canonical schema is declared non-nullable (see Schema.hpp), so a column with
// a null is not a batch this codec has a representation for — TickRecord has
// no "absent" state for a price. Validate() checks lengths and types but not
// nullability against the schema, so this is a separate loop.
arrow::Status requireCanonicalBatch(const arrow::RecordBatch& batch,
                                    const std::shared_ptr<arrow::Schema>& expected,
                                    const char* what) {
  if (expected == nullptr) {
    return arrow::Status::Invalid("gma::data: no canonical schema for ", what);
  }
  if (!batch.schema()->Equals(*expected, /*check_metadata=*/false)) {
    return arrow::Status::Invalid(
        "gma::data: batch schema does not match the canonical ", what,
        " schema.\n  got:      ", batch.schema()->ToString(),
        "\n  expected: ", expected->ToString());
  }
  const auto version = schemaVersionOf(*batch.schema());
  if (version != kSchemaVersion) {
    return arrow::Status::Invalid(
        "gma::data: batch declares ", kSchemaVersionKey, "=", version,
        " but this build understands ", kSchemaVersion,
        " — refusing to guess a column layout");
  }
  ARROW_RETURN_NOT_OK(batch.Validate());
  for (int c = 0; c < batch.num_columns(); ++c) {
    const auto& col = *batch.column(c);
    if (col.null_count() != 0) {
      return arrow::Status::Invalid(
          "gma::data: ", what, " column '", batch.schema()->field(c)->name(),
          "' holds ", col.null_count(),
          " null(s), but every column of every canonical schema is declared "
          "non-nullable and the PODs have no absent state");
    }
  }
  return arrow::Status::OK();
}

// Shared header columns, fetched once per batch.
struct HeaderColumns {
  const arrow::TimestampArray* tsEvent = nullptr;
  const arrow::TimestampArray* tsRecv  = nullptr;
  const arrow::UInt64Array* providerSeq = nullptr;
  const arrow::UInt32Array* sessionId   = nullptr;
  const arrow::UInt32Array* instrumentId = nullptr;
  std::shared_ptr<arrow::Array> symbol;
  std::shared_ptr<arrow::Array> venue;
  std::vector<SymbolId> symbolIds;
  std::vector<SymbolId> venueIds;

  arrow::Status bind(const arrow::RecordBatch& batch, SymbolTable& symbols,
                     SymbolTable& venues) {
    ARROW_ASSIGN_OR_RAISE(tsEvent, column<arrow::TimestampArray>(batch, "ts_event"));
    ARROW_ASSIGN_OR_RAISE(tsRecv, column<arrow::TimestampArray>(batch, "ts_recv"));
    ARROW_ASSIGN_OR_RAISE(providerSeq, column<arrow::UInt64Array>(batch, "provider_seq"));
    ARROW_ASSIGN_OR_RAISE(sessionId, column<arrow::UInt32Array>(batch, "session_id"));
    ARROW_ASSIGN_OR_RAISE(instrumentId, column<arrow::UInt32Array>(batch, "instrument_id"));
    symbol = batch.GetColumnByName("symbol");
    venue  = batch.GetColumnByName("venue");
    if (symbol == nullptr || venue == nullptr) {
      return arrow::Status::Invalid("gma::data: batch is missing symbol or venue");
    }
    ARROW_ASSIGN_OR_RAISE(symbolIds, internDictionary(*symbol, symbols, "symbol"));
    ARROW_ASSIGN_OR_RAISE(venueIds, internDictionary(*venue, venues, "venue"));
    return arrow::Status::OK();
  }

  arrow::Result<RecordHeader> at(std::int64_t i) const {
    RecordHeader h;
    h.tsEventNs = tsEvent->Value(i);
    h.tsRecvNs = tsRecv->Value(i);
    h.providerSeq = providerSeq->Value(i);
    h.sessionId = sessionId->Value(i);
    h.instrumentId = instrumentId->Value(i);
    ARROW_ASSIGN_OR_RAISE(h.symbolId, dictIdAt(*symbol, symbolIds, i, "symbol"));
    ARROW_ASSIGN_OR_RAISE(h.venueId, dictIdAt(*venue, venueIds, i, "venue"));
    return h;
  }
};

// Mirror of HeaderColumns for the encode direction.
struct HeaderBuilders {
  arrow::TimestampBuilder tsEvent;
  arrow::TimestampBuilder tsRecv;
  arrow::UInt64Builder providerSeq;
  arrow::UInt32Builder sessionId;
  arrow::UInt32Builder instrumentId;
  SymbolColumn symbol;
  VenueColumn venue;

  HeaderBuilders(const SymbolTable& symbols, const SymbolTable& venues,
                 arrow::MemoryPool* pool)
      : tsEvent(nanosUtc(), pool), tsRecv(nanosUtc(), pool),
        providerSeq(pool), sessionId(pool), instrumentId(pool),
        symbol(symbolType(), symbols, pool, "symbol"),
        venue(venueType(), venues, pool, "venue") {}

  arrow::Status append(const RecordHeader& h) {
    ARROW_RETURN_NOT_OK(tsEvent.Append(h.tsEventNs));
    ARROW_RETURN_NOT_OK(tsRecv.Append(h.tsRecvNs));
    ARROW_RETURN_NOT_OK(providerSeq.Append(h.providerSeq));
    ARROW_RETURN_NOT_OK(sessionId.Append(h.sessionId));
    ARROW_RETURN_NOT_OK(instrumentId.Append(h.instrumentId));
    ARROW_RETURN_NOT_OK(symbol.append(h.symbolId));
    return venue.append(h.venueId);
  }

  // Appends the seven common arrays, in schema order, to `out`.
  arrow::Status finish(arrow::ArrayVector* out) {
    std::shared_ptr<arrow::Array> a;
    ARROW_RETURN_NOT_OK(tsEvent.Finish(&a));      out->push_back(a);
    ARROW_RETURN_NOT_OK(tsRecv.Finish(&a));       out->push_back(a);
    ARROW_RETURN_NOT_OK(providerSeq.Finish(&a));  out->push_back(a);
    ARROW_RETURN_NOT_OK(sessionId.Finish(&a));    out->push_back(a);
    ARROW_ASSIGN_OR_RAISE(a, symbol.finish());    out->push_back(a);
    ARROW_RETURN_NOT_OK(instrumentId.Finish(&a)); out->push_back(a);
    ARROW_ASSIGN_OR_RAISE(a, venue.finish());     out->push_back(a);
    return arrow::Status::OK();
  }
};

// Rolls `out` back to the size it had on entry unless the decode is dismissed.
//
// The contract (Schema.hpp) is that `out` is APPENDED TO, not cleared, so a
// caller may accumulate across batches. That only works if a REFUSED batch
// contributes nothing: otherwise a caller that accumulates and checks the
// Status at the end is holding rows [0, i) of a batch the codec rejected
// mid-loop. The enum and dictionary checks above make mid-loop refusal a
// routine wire-input outcome rather than a corrupt-memory edge case, so the
// decoders are all-or-nothing about their own appends.
template <typename T>
class AppendRollback {
 public:
  explicit AppendRollback(std::vector<T>& out)
      : out_(out), mark_(out.size()) {}
  ~AppendRollback() {
    if (!dismissed_ && out_.size() > mark_) {
      out_.resize(mark_);
    }
  }
  void dismiss() { dismissed_ = true; }

  AppendRollback(const AppendRollback&) = delete;
  AppendRollback& operator=(const AppendRollback&) = delete;

 private:
  std::vector<T>& out_;
  std::size_t mark_;
  bool dismissed_ = false;
};

arrow::MemoryPool* poolOr(arrow::MemoryPool* pool) {
  return pool != nullptr ? pool : arrow::default_memory_pool();
}

} // namespace

// ---------------------------------------------------------------------------
// Schemas
// ---------------------------------------------------------------------------

std::shared_ptr<arrow::Schema> tradesSchema() {
  static const auto s = makeSchema({
      arrow::field("price", arrow::int64(), /*nullable=*/false),
      arrow::field("size", arrow::uint64(), false),
      arrow::field("aggressor", arrow::uint8(), false),
      arrow::field("book_neutral", arrow::boolean(), false),
      arrow::field("trade_id", arrow::uint64(), false),
      arrow::field("flags", arrow::uint16(), false),
  });
  return s;
}

std::shared_ptr<arrow::Schema> quotesSchema() {
  static const auto s = makeSchema({
      arrow::field("bid_px", arrow::int64(), /*nullable=*/false),
      arrow::field("ask_px", arrow::int64(), false),
      arrow::field("bid_sz", arrow::uint64(), false),
      arrow::field("ask_sz", arrow::uint64(), false),
      arrow::field("bid_ct", arrow::uint32(), false),
      arrow::field("ask_ct", arrow::uint32(), false),
      arrow::field("flags", arrow::uint16(), false),
  });
  return s;
}

std::shared_ptr<arrow::Schema> bookEventsSchema() {
  static const auto s = makeSchema({
      arrow::field("action", arrow::uint8(), /*nullable=*/false),
      arrow::field("side", arrow::uint8(), false),
      arrow::field("price", arrow::int64(), false),
      arrow::field("size", arrow::uint64(), false),
      arrow::field("order_id", arrow::uint64(), false),
      arrow::field("priority", arrow::uint64(), false),
      arrow::field("depth", arrow::uint16(), false),
      arrow::field("epoch", arrow::uint32(), false),
      arrow::field("flags", arrow::uint16(), false),
  });
  return s;
}

std::shared_ptr<arrow::Schema> bookSnapshotsSchema() {
  static const auto s = makeSchema({
      // The resume seq the snapshot was taken at — OrderBookManager's
      // onSnapshotPerOrder(symbol, orders, snapshotSeq). Shared by every row
      // of one snapshot, which is what makes the rows a snapshot rather than
      // a pile of levels (docs/data-layer.md §8.5).
      arrow::field("snapshot_seq", arrow::uint64(), /*nullable=*/false),
      arrow::field("side", arrow::uint8(), false),
      arrow::field("price", arrow::int64(), false),
      arrow::field("size", arrow::uint64(), false),
      arrow::field("order_count", arrow::uint32(), false),
  });
  return s;
}

std::shared_ptr<arrow::Schema> schemaFor(RecordKind kind) {
  switch (kind) {
    case RecordKind::Trade:        return tradesSchema();
    case RecordKind::Quote:        return quotesSchema();
    case RecordKind::BookEvent:    return bookEventsSchema();
    case RecordKind::BookSnapshot: return bookSnapshotsSchema();
  }
  return nullptr;
}

std::int32_t schemaVersionOf(const arrow::Schema& schema) {
  const auto& md = schema.metadata();
  if (md == nullptr) return -1;
  const auto raw = md->Get(std::string(kSchemaVersionKey));
  if (!raw.ok()) return -1;
  const std::string& text = *raw;
  std::int32_t out = 0;
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto res = std::from_chars(first, last, out);
  if (res.ec != std::errc{} || res.ptr != last) return -1;
  return out;
}

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

namespace {

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeTicksImpl(
    const std::vector<TickRecord>& records, RecordKind kind,
    const SymbolTable& symbols, const SymbolTable& venues,
    arrow::MemoryPool* pool) {
  auto schema = schemaFor(kind);
  if (schema == nullptr) {
    return arrow::Status::Invalid("gma::data: encodeTicks: unknown RecordKind");
  }
  pool = poolOr(pool);
  HeaderBuilders hdr(symbols, venues, pool);

  const bool trades = (kind == RecordKind::Trade);

  arrow::Int64Builder price(pool), bidPx(pool), askPx(pool);
  arrow::UInt64Builder size(pool), tradeId(pool), bidSz(pool), askSz(pool);
  arrow::UInt8Builder aggressor(pool);
  arrow::BooleanBuilder bookNeutral(pool);
  arrow::UInt32Builder bidCt(pool), askCt(pool);
  arrow::UInt16Builder flags(pool);

  for (const auto& r : records) {
    if (r.kind != kind) {
      return arrow::Status::Invalid(
          "gma::data: encodeTicks was asked for kind ",
          static_cast<int>(kind), " but a record carries kind ",
          static_cast<int>(r.kind),
          " — a mixed vector would silently drop half its fields");
    }
    ARROW_RETURN_NOT_OK(hdr.append(r.header));
    if (trades) {
      ARROW_RETURN_NOT_OK(price.Append(r.price));
      ARROW_RETURN_NOT_OK(size.Append(r.size));
      ARROW_RETURN_NOT_OK(aggressor.Append(static_cast<std::uint8_t>(r.aggressor)));
      ARROW_RETURN_NOT_OK(bookNeutral.Append(r.bookNeutral));
      ARROW_RETURN_NOT_OK(tradeId.Append(r.tradeId));
    } else {
      ARROW_RETURN_NOT_OK(bidPx.Append(r.bidPx));
      ARROW_RETURN_NOT_OK(askPx.Append(r.askPx));
      ARROW_RETURN_NOT_OK(bidSz.Append(r.bidSz));
      ARROW_RETURN_NOT_OK(askSz.Append(r.askSz));
      ARROW_RETURN_NOT_OK(bidCt.Append(r.bidCt));
      ARROW_RETURN_NOT_OK(askCt.Append(r.askCt));
    }
    ARROW_RETURN_NOT_OK(flags.Append(r.flags));
  }

  arrow::ArrayVector cols;
  ARROW_RETURN_NOT_OK(hdr.finish(&cols));
  std::shared_ptr<arrow::Array> a;
  if (trades) {
    ARROW_RETURN_NOT_OK(price.Finish(&a));       cols.push_back(a);
    ARROW_RETURN_NOT_OK(size.Finish(&a));        cols.push_back(a);
    ARROW_RETURN_NOT_OK(aggressor.Finish(&a));   cols.push_back(a);
    ARROW_RETURN_NOT_OK(bookNeutral.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(tradeId.Finish(&a));     cols.push_back(a);
  } else {
    ARROW_RETURN_NOT_OK(bidPx.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(askPx.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(bidSz.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(askSz.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(bidCt.Finish(&a)); cols.push_back(a);
    ARROW_RETURN_NOT_OK(askCt.Finish(&a)); cols.push_back(a);
  }
  ARROW_RETURN_NOT_OK(flags.Finish(&a)); cols.push_back(a);

  return arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(records.size()),
                                  std::move(cols));
}

} // namespace

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeTrades(
    const std::vector<TickRecord>& records, const SymbolTable& symbols,
    const SymbolTable& venues, arrow::MemoryPool* pool) {
  return encodeTicksImpl(records, RecordKind::Trade, symbols, venues, pool);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeQuotes(
    const std::vector<TickRecord>& records, const SymbolTable& symbols,
    const SymbolTable& venues, arrow::MemoryPool* pool) {
  return encodeTicksImpl(records, RecordKind::Quote, symbols, venues, pool);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeBookEvents(
    const std::vector<BookRecord>& records, const SymbolTable& symbols,
    const SymbolTable& venues, arrow::MemoryPool* pool) {
  pool = poolOr(pool);
  HeaderBuilders hdr(symbols, venues, pool);
  arrow::UInt8Builder action(pool), side(pool);
  arrow::Int64Builder price(pool);
  arrow::UInt64Builder size(pool), orderId(pool), priority(pool);
  arrow::UInt16Builder depth(pool), flags(pool);
  arrow::UInt32Builder epoch(pool);

  for (const auto& r : records) {
    ARROW_RETURN_NOT_OK(hdr.append(r.header));
    ARROW_RETURN_NOT_OK(action.Append(static_cast<std::uint8_t>(r.action)));
    ARROW_RETURN_NOT_OK(side.Append(static_cast<std::uint8_t>(r.side)));
    ARROW_RETURN_NOT_OK(price.Append(r.price));
    ARROW_RETURN_NOT_OK(size.Append(r.size));
    ARROW_RETURN_NOT_OK(orderId.Append(r.orderId));
    ARROW_RETURN_NOT_OK(priority.Append(r.priority));
    ARROW_RETURN_NOT_OK(depth.Append(r.depth));
    ARROW_RETURN_NOT_OK(epoch.Append(r.epoch));
    ARROW_RETURN_NOT_OK(flags.Append(r.flags));
  }

  arrow::ArrayVector cols;
  ARROW_RETURN_NOT_OK(hdr.finish(&cols));
  std::shared_ptr<arrow::Array> a;
  ARROW_RETURN_NOT_OK(action.Finish(&a));   cols.push_back(a);
  ARROW_RETURN_NOT_OK(side.Finish(&a));     cols.push_back(a);
  ARROW_RETURN_NOT_OK(price.Finish(&a));    cols.push_back(a);
  ARROW_RETURN_NOT_OK(size.Finish(&a));     cols.push_back(a);
  ARROW_RETURN_NOT_OK(orderId.Finish(&a));  cols.push_back(a);
  ARROW_RETURN_NOT_OK(priority.Finish(&a)); cols.push_back(a);
  ARROW_RETURN_NOT_OK(depth.Finish(&a));    cols.push_back(a);
  ARROW_RETURN_NOT_OK(epoch.Finish(&a));    cols.push_back(a);
  ARROW_RETURN_NOT_OK(flags.Finish(&a));    cols.push_back(a);

  return arrow::RecordBatch::Make(bookEventsSchema(),
                                  static_cast<std::int64_t>(records.size()),
                                  std::move(cols));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeBookSnapshots(
    const std::vector<SnapshotRecord>& records, const SymbolTable& symbols,
    const SymbolTable& venues, arrow::MemoryPool* pool) {
  pool = poolOr(pool);
  HeaderBuilders hdr(symbols, venues, pool);
  arrow::UInt64Builder snapshotSeq(pool), size(pool);
  arrow::UInt8Builder side(pool);
  arrow::Int64Builder price(pool);
  arrow::UInt32Builder orderCount(pool);

  for (const auto& r : records) {
    ARROW_RETURN_NOT_OK(hdr.append(r.header));
    ARROW_RETURN_NOT_OK(snapshotSeq.Append(r.snapshotSeq));
    ARROW_RETURN_NOT_OK(side.Append(static_cast<std::uint8_t>(r.level.side)));
    ARROW_RETURN_NOT_OK(price.Append(r.level.price));
    ARROW_RETURN_NOT_OK(size.Append(r.level.size));
    ARROW_RETURN_NOT_OK(orderCount.Append(r.level.orderCount));
  }

  arrow::ArrayVector cols;
  ARROW_RETURN_NOT_OK(hdr.finish(&cols));
  std::shared_ptr<arrow::Array> a;
  ARROW_RETURN_NOT_OK(snapshotSeq.Finish(&a)); cols.push_back(a);
  ARROW_RETURN_NOT_OK(side.Finish(&a));        cols.push_back(a);
  ARROW_RETURN_NOT_OK(price.Finish(&a));       cols.push_back(a);
  ARROW_RETURN_NOT_OK(size.Finish(&a));        cols.push_back(a);
  ARROW_RETURN_NOT_OK(orderCount.Finish(&a));  cols.push_back(a);

  return arrow::RecordBatch::Make(bookSnapshotsSchema(),
                                  static_cast<std::int64_t>(records.size()),
                                  std::move(cols));
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

arrow::Status decodeTicks(const arrow::RecordBatch& batch, RecordKind kind,
                          SymbolTable& symbols, SymbolTable& venues,
                          std::vector<TickRecord>& out) {
  if (kind != RecordKind::Trade && kind != RecordKind::Quote) {
    return arrow::Status::Invalid(
        "gma::data: decodeTicks handles Trade and Quote only; "
        "book kinds go through decodeBookEvents / decodeBookSnapshots");
  }
  ARROW_RETURN_NOT_OK(requireCanonicalBatch(
      batch, schemaFor(kind),
      kind == RecordKind::Trade ? "trades" : "quotes"));
  HeaderColumns hdr;
  ARROW_RETURN_NOT_OK(hdr.bind(batch, symbols, venues));

  const arrow::Int64Array* price = nullptr;
  const arrow::UInt64Array* size = nullptr;
  const arrow::UInt8Array* aggressor = nullptr;
  const arrow::BooleanArray* bookNeutral = nullptr;
  const arrow::UInt64Array* tradeId = nullptr;
  const arrow::Int64Array* bidPx = nullptr;
  const arrow::Int64Array* askPx = nullptr;
  const arrow::UInt64Array* bidSz = nullptr;
  const arrow::UInt64Array* askSz = nullptr;
  const arrow::UInt32Array* bidCt = nullptr;
  const arrow::UInt32Array* askCt = nullptr;
  const arrow::UInt16Array* flags = nullptr;
  ARROW_ASSIGN_OR_RAISE(flags, column<arrow::UInt16Array>(batch, "flags"));

  const bool trades = (kind == RecordKind::Trade);
  if (trades) {
    ARROW_ASSIGN_OR_RAISE(price, column<arrow::Int64Array>(batch, "price"));
    ARROW_ASSIGN_OR_RAISE(size, column<arrow::UInt64Array>(batch, "size"));
    ARROW_ASSIGN_OR_RAISE(aggressor, column<arrow::UInt8Array>(batch, "aggressor"));
    ARROW_ASSIGN_OR_RAISE(bookNeutral, column<arrow::BooleanArray>(batch, "book_neutral"));
    ARROW_ASSIGN_OR_RAISE(tradeId, column<arrow::UInt64Array>(batch, "trade_id"));
  } else {
    ARROW_ASSIGN_OR_RAISE(bidPx, column<arrow::Int64Array>(batch, "bid_px"));
    ARROW_ASSIGN_OR_RAISE(askPx, column<arrow::Int64Array>(batch, "ask_px"));
    ARROW_ASSIGN_OR_RAISE(bidSz, column<arrow::UInt64Array>(batch, "bid_sz"));
    ARROW_ASSIGN_OR_RAISE(askSz, column<arrow::UInt64Array>(batch, "ask_sz"));
    ARROW_ASSIGN_OR_RAISE(bidCt, column<arrow::UInt32Array>(batch, "bid_ct"));
    ARROW_ASSIGN_OR_RAISE(askCt, column<arrow::UInt32Array>(batch, "ask_ct"));
  }

  AppendRollback<TickRecord> rollback(out);
  out.reserve(out.size() + static_cast<std::size_t>(batch.num_rows()));
  for (std::int64_t i = 0; i < batch.num_rows(); ++i) {
    TickRecord r;
    ARROW_ASSIGN_OR_RAISE(r.header, hdr.at(i));
    r.kind = kind;
    if (trades) {
      r.price = price->Value(i);
      r.size = size->Value(i);
      ARROW_ASSIGN_OR_RAISE(r.aggressor,
                            enumAt<Aggressor>(*aggressor, i,
                                              kMaxAggressorValue, "aggressor"));
      r.bookNeutral = bookNeutral->Value(i);
      r.tradeId = tradeId->Value(i);
    } else {
      r.bidPx = bidPx->Value(i);
      r.askPx = askPx->Value(i);
      r.bidSz = bidSz->Value(i);
      r.askSz = askSz->Value(i);
      r.bidCt = bidCt->Value(i);
      r.askCt = askCt->Value(i);
    }
    r.flags = flags->Value(i);
    out.push_back(r);
  }
  rollback.dismiss();
  return arrow::Status::OK();
}

arrow::Status decodeBookEvents(const arrow::RecordBatch& batch,
                               SymbolTable& symbols, SymbolTable& venues,
                               std::vector<BookRecord>& out) {
  ARROW_RETURN_NOT_OK(requireCanonicalBatch(batch, bookEventsSchema(), "book_events"));
  HeaderColumns hdr;
  ARROW_RETURN_NOT_OK(hdr.bind(batch, symbols, venues));

  ARROW_ASSIGN_OR_RAISE(const auto* action, column<arrow::UInt8Array>(batch, "action"));
  ARROW_ASSIGN_OR_RAISE(const auto* side, column<arrow::UInt8Array>(batch, "side"));
  ARROW_ASSIGN_OR_RAISE(const auto* price, column<arrow::Int64Array>(batch, "price"));
  ARROW_ASSIGN_OR_RAISE(const auto* size, column<arrow::UInt64Array>(batch, "size"));
  ARROW_ASSIGN_OR_RAISE(const auto* orderId, column<arrow::UInt64Array>(batch, "order_id"));
  ARROW_ASSIGN_OR_RAISE(const auto* priority, column<arrow::UInt64Array>(batch, "priority"));
  ARROW_ASSIGN_OR_RAISE(const auto* depth, column<arrow::UInt16Array>(batch, "depth"));
  ARROW_ASSIGN_OR_RAISE(const auto* epoch, column<arrow::UInt32Array>(batch, "epoch"));
  ARROW_ASSIGN_OR_RAISE(const auto* flags, column<arrow::UInt16Array>(batch, "flags"));

  AppendRollback<BookRecord> rollback(out);
  out.reserve(out.size() + static_cast<std::size_t>(batch.num_rows()));
  for (std::int64_t i = 0; i < batch.num_rows(); ++i) {
    BookRecord r;
    ARROW_ASSIGN_OR_RAISE(r.header, hdr.at(i));
    ARROW_ASSIGN_OR_RAISE(r.action,
                          enumAt<BookAction>(*action, i, kMaxBookActionValue,
                                             "action"));
    ARROW_ASSIGN_OR_RAISE(r.side,
                          enumAt<Side>(*side, i, kMaxSideValue, "side"));
    r.price = price->Value(i);
    r.size = size->Value(i);
    r.orderId = orderId->Value(i);
    r.priority = priority->Value(i);
    r.depth = depth->Value(i);
    r.epoch = epoch->Value(i);
    r.flags = flags->Value(i);
    out.push_back(r);
  }
  rollback.dismiss();
  return arrow::Status::OK();
}

arrow::Status decodeBookSnapshots(const arrow::RecordBatch& batch,
                                  SymbolTable& symbols, SymbolTable& venues,
                                  std::vector<SnapshotRecord>& out) {
  ARROW_RETURN_NOT_OK(requireCanonicalBatch(batch, bookSnapshotsSchema(), "book_snapshots"));
  HeaderColumns hdr;
  ARROW_RETURN_NOT_OK(hdr.bind(batch, symbols, venues));

  ARROW_ASSIGN_OR_RAISE(const auto* seq, column<arrow::UInt64Array>(batch, "snapshot_seq"));
  ARROW_ASSIGN_OR_RAISE(const auto* side, column<arrow::UInt8Array>(batch, "side"));
  ARROW_ASSIGN_OR_RAISE(const auto* price, column<arrow::Int64Array>(batch, "price"));
  ARROW_ASSIGN_OR_RAISE(const auto* size, column<arrow::UInt64Array>(batch, "size"));
  ARROW_ASSIGN_OR_RAISE(const auto* count, column<arrow::UInt32Array>(batch, "order_count"));

  AppendRollback<SnapshotRecord> rollback(out);
  out.reserve(out.size() + static_cast<std::size_t>(batch.num_rows()));
  for (std::int64_t i = 0; i < batch.num_rows(); ++i) {
    SnapshotRecord r;
    ARROW_ASSIGN_OR_RAISE(r.header, hdr.at(i));
    r.snapshotSeq = seq->Value(i);
    ARROW_ASSIGN_OR_RAISE(r.level.side,
                          enumAt<Side>(*side, i, kMaxSideValue, "side"));
    r.level.price = price->Value(i);
    r.level.size = size->Value(i);
    r.level.orderCount = count->Value(i);
    out.push_back(r);
  }
  rollback.dismiss();
  return arrow::Status::OK();
}

} // namespace gma::data
