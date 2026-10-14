#include "gma/data/DatabentoAdapter.hpp"

#include <arrow/record_batch.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

namespace gma::data {
namespace {

// ---------------------------------------------------------------------------
// Little-endian loads.
//
// Byte-at-a-time, not a memcpy into the struct: the DBN records are PACKED
// (`price` sits at offset 24 of a 56-byte record that may start at any byte of
// a read buffer), so a typed load would be unaligned, and a memcpy would still
// assume a little-endian host. These are the whole endianness story.
// ---------------------------------------------------------------------------

constexpr std::uint8_t u8at(const std::byte* p, std::size_t i) noexcept {
  return std::to_integer<std::uint8_t>(p[i]);
}

std::uint16_t loadU16(const std::byte* p) noexcept {
  return static_cast<std::uint16_t>(u8at(p, 0)) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(u8at(p, 1)) << 8);
}

std::uint32_t loadU32(const std::byte* p) noexcept {
  std::uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(u8at(p, i)) << (8 * i);
  return v;
}

std::uint64_t loadU64(const std::byte* p) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(u8at(p, i)) << (8 * i);
  return v;
}

std::int32_t loadI32(const std::byte* p) noexcept {
  return static_cast<std::int32_t>(loadU32(p));
}

std::int64_t loadI64(const std::byte* p) noexcept {
  return static_cast<std::int64_t>(loadU64(p));
}

char loadChar(const std::byte* p) noexcept { return static_cast<char>(u8at(p, 0)); }

// A fixed-width, NUL-padded C string field. DBN pads with zeros and does NOT
// guarantee a terminator when the string exactly fills the field, so the scan
// is bounded by the field rather than by a NUL.
std::string readCstr(const std::byte* p, std::size_t width) {
  std::size_t n = 0;
  while (n < width && u8at(p, n) != 0) ++n;
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(u8at(p, i));
  return s;
}

// ---------------------------------------------------------------------------
// Calendar. Only used for symbology: a DBN mapping interval is a yyyymmdd date
// range and a record carries nanoseconds, so one of them has to move.
//
// This is NOT a value computation in the SPEC D1 sense — no operator input
// passes through it and nothing it returns reaches a price, a size or a
// timestamp column. It decides which TICKER a row carries. Integer-only, and
// `std::chrono`'s calendar types are avoided so the arithmetic is visible.
// Howard Hinnant's `civil_from_days`, which is public domain.
// ---------------------------------------------------------------------------

inline constexpr std::int64_t kNsPerDay = 86'400'000'000'000LL;

std::uint32_t yyyymmddFromDays(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<std::uint64_t>(z - era * 146097);
  const std::uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const std::uint64_t mp = (5 * doy + 2) / 153;
  const std::uint64_t d = doy - (153 * mp + 2) / 5 + 1;
  const std::uint64_t m = mp < 10 ? mp + 3 : mp - 9;
  const std::int64_t year = y + static_cast<std::int64_t>(m <= 2);
  return static_cast<std::uint32_t>(year * 10000 + static_cast<std::int64_t>(m) * 100 +
                                    static_cast<std::int64_t>(d));
}

// Floor division, so a pre-1970 timestamp lands on the right day rather than
// one day late.
std::uint32_t yyyymmddFromNs(std::int64_t ns) noexcept {
  const std::int64_t days =
      ns >= 0 ? ns / kNsPerDay : -((-ns + kNsPerDay - 1) / kNsPerDay);
  return yyyymmddFromDays(days);
}

// ---------------------------------------------------------------------------
// A bounds-checked cursor over the metadata block. Every read is checked, and
// the first overrun latches `bad_` so the caller checks once at the end
// instead of after every field.
// ---------------------------------------------------------------------------

class Reader {
 public:
  explicit Reader(std::span<const std::byte> s) : s_(s) {}

  bool bad() const noexcept { return bad_; }
  std::size_t offset() const noexcept { return off_; }
  std::size_t remaining() const noexcept { return bad_ ? 0 : s_.size() - off_; }

  const std::byte* take(std::size_t n) noexcept {
    if (bad_ || s_.size() - off_ < n) { bad_ = true; return nullptr; }
    const std::byte* p = s_.data() + off_;
    off_ += n;
    return p;
  }
  void skip(std::size_t n) noexcept { take(n); }
  std::uint8_t  u8()  noexcept { const auto* p = take(1); return p ? u8at(p, 0) : 0; }
  std::uint16_t u16() noexcept { const auto* p = take(2); return p ? loadU16(p) : 0; }
  std::uint32_t u32() noexcept { const auto* p = take(4); return p ? loadU32(p) : 0; }
  std::uint64_t u64() noexcept { const auto* p = take(8); return p ? loadU64(p) : 0; }
  std::string cstr(std::size_t width) {
    const auto* p = take(width);
    return p ? readCstr(p, width) : std::string{};
  }

 private:
  std::span<const std::byte> s_;
  std::size_t off_ = 0;
  bool bad_ = false;
};

std::string hex2(std::uint8_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  return std::string{'0', 'x', kDigits[(v >> 4) & 0xF], kDigits[v & 0xF]};
}

// A char rendered for an error message: printable as itself, otherwise hex.
std::string showChar(char c) {
  const auto u = static_cast<std::uint8_t>(c);
  if (u >= 0x20 && u < 0x7F) return std::string{'\'', c, '\''};
  return hex2(u);
}

bool parseU32(std::string_view s, std::uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  std::uint64_t v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<std::uint64_t>(c - '0');
    if (v > 0xFFFFFFFFULL) return false;
  }
  out = static_cast<std::uint32_t>(v);
  return true;
}

}  // namespace

// ===========================================================================
// dbn — declared-domain predicates
// ===========================================================================
namespace dbn {

bool isDeclaredAction(char c) noexcept {
  for (const Action a : kAllActions) {
    if (static_cast<char>(a) == c) return true;
  }
  return false;
}

bool isDeclaredSide(char c) noexcept {
  for (const Side s : kAllSides) {
    if (static_cast<char>(s) == c) return true;
  }
  return false;
}

bool isDeclaredRType(std::uint8_t r) noexcept {
  for (const RType t : kAllRTypes) {
    if (static_cast<std::uint8_t>(t) == r) return true;
  }
  return false;
}

const char* actionName(char c) noexcept {
  switch (c) {
    case static_cast<char>(Action::Modify): return "Modify";
    case static_cast<char>(Action::Trade):  return "Trade";
    case static_cast<char>(Action::Fill):   return "Fill";
    case static_cast<char>(Action::Cancel): return "Cancel";
    case static_cast<char>(Action::Add):    return "Add";
    case static_cast<char>(Action::Clear):  return "Clear";
    case static_cast<char>(Action::None):   return "None";
    default: return "?";
  }
}

const char* sideName(char c) noexcept {
  switch (c) {
    case static_cast<char>(Side::Ask):  return "Ask";
    case static_cast<char>(Side::Bid):  return "Bid";
    case static_cast<char>(Side::None): return "None";
    default: return "?";
  }
}

}  // namespace dbn

// ===========================================================================
// DatabentoAdapter
// ===========================================================================

DatabentoAdapter::DatabentoAdapter(Config cfg, SymbolTable& symbols, SymbolTable& venues)
    : cfg_(std::move(cfg)), symbols_(symbols), venues_(venues) {
  if (!cfg_.venue.empty()) venueId_ = venues_.intern(cfg_.venue);
  if (!cfg_.symbolOverride.empty()) overrideSymbolId_ = symbols_.intern(cfg_.symbolOverride);
  if (cfg_.maxTrackedOrders == 0) cfg_.maxTrackedOrders = 1;
}

std::vector<TickRecord> DatabentoAdapter::takeTrades() {
  std::vector<TickRecord> out;
  out.swap(ticks_);
  return out;
}

std::vector<BookRecord> DatabentoAdapter::takeBookEvents() {
  std::vector<BookRecord> out;
  out.swap(book_);
  return out;
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> DatabentoAdapter::tradesBatch(
    arrow::MemoryPool* pool) const {
  return encodeTrades(ticks_, symbols_, venues_, pool);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> DatabentoAdapter::bookEventsBatch(
    arrow::MemoryPool* pool) const {
  return encodeBookEvents(book_, symbols_, venues_, pool);
}

std::uint32_t DatabentoAdapter::epochOf(std::uint32_t instrumentId) const {
  const auto it = epochs_.find(instrumentId);
  return it == epochs_.end() ? 0u : it->second;
}

std::optional<std::uint64_t> DatabentoAdapter::restingSize(std::uint32_t instrumentId,
                                                           std::uint64_t orderId) const {
  const auto it = orders_.find(OrderKey{instrumentId, orderId});
  if (it == orders_.end()) return std::nullopt;
  return it->second;
}

void DatabentoAdapter::trackOrder(const OrderKey& k, std::uint64_t size) {
  // ItchAdapter::routeAddOrder's M4 bound, verbatim in intent: evict the
  // oldest bucket entry once the cap is hit. Dropping tracking only forfeits
  // delta resolution for that order; memory stays bounded.
  if (orders_.size() >= cfg_.maxTrackedOrders && orders_.find(k) == orders_.end()) {
    orders_.erase(orders_.begin());
    ++stats_.evictedOrders;
  }
  orders_[k] = size;
}

// ---------------------------------------------------------------------------
// feed / finish / drain — framing
// ---------------------------------------------------------------------------

arrow::Status DatabentoAdapter::feed(std::span<const std::byte> raw) {
  if (!poison_.ok()) return poison_;
  stats_.bytesIn += raw.size();
  buf_.insert(buf_.end(), raw.begin(), raw.end());
  const arrow::Status st = drain();
  if (!st.ok()) poison_ = st;
  return st;
}

arrow::Status DatabentoAdapter::finish() {
  if (!poison_.ok()) return poison_;
  if (!haveMeta_) {
    poison_ = arrow::Status::Invalid(
        "DBN stream ended before its metadata block was complete (",
        pendingBytes(), " byte(s) held); this is a truncated file, not an empty one");
    return poison_;
  }
  if (pendingBytes() != 0) {
    // IProviderAdapter::flushPartial's contract: a partial record at the end
    // of a stream is a GAP and must be reported, never held until later bytes
    // silently complete it.
    poison_ = arrow::Status::Invalid(
        "DBN stream ended mid-record: ", pendingBytes(),
        " byte(s) of an incomplete record are held. That is a gap, not an "
        "end-of-file.");
    return poison_;
  }
  return arrow::Status::OK();
}

arrow::Status DatabentoAdapter::drain() {
  if (!haveMeta_) {
    const std::size_t avail = pendingBytes();
    if (avail < dbn::kPrefixLen + dbn::kMetadataLenLen) return arrow::Status::OK();
    const std::byte* p = buf_.data() + cursor_;

    if (u8at(p, 0) == dbn::kZstdMagic[0] && u8at(p, 1) == dbn::kZstdMagic[1] &&
        u8at(p, 2) == dbn::kZstdMagic[2] && u8at(p, 3) == dbn::kZstdMagic[3]) {
      return arrow::Status::Invalid(
          "this is a zstd frame, not plain DBN. `.dbn.zst` must be decompressed "
          "before it reaches DatabentoAdapter — zstd is deliberately not a "
          "dependency of gma_data (SPEC Q14).");
    }
    if (loadChar(p) != dbn::kMagic[0] || loadChar(p + 1) != dbn::kMagic[1] ||
        loadChar(p + 2) != dbn::kMagic[2]) {
      return arrow::Status::Invalid("not a DBN stream: expected magic \"DBN\", got ",
                                    hex2(u8at(p, 0)), " ", hex2(u8at(p, 1)), " ",
                                    hex2(u8at(p, 2)));
    }
    const std::uint8_t version = u8at(p, 3);
    if (version < dbn::kMinVersion || version > dbn::kMaxVersion) {
      return arrow::Status::Invalid(
          "unsupported DBN version ", static_cast<int>(version), " (supported: ",
          static_cast<int>(dbn::kMinVersion), "..", static_cast<int>(dbn::kMaxVersion),
          "). Version 0 denotes a legacy DBZ file. An unknown version is REFUSED "
          "rather than read with a guessed layout.");
    }
    const std::size_t mlen = loadU32(p + dbn::kPrefixLen);
    // The smallest legal variable section: layout_length plus the four counts.
    constexpr std::size_t kMinVarLen = 5 * sizeof(std::uint32_t);
    if (mlen < dbn::kFixedMetadataLen + kMinVarLen) {
      return arrow::Status::Invalid("DBN metadata length ", mlen,
                                    " is shorter than the minimum ",
                                    dbn::kFixedMetadataLen + kMinVarLen);
    }
    if (avail < dbn::kPrefixLen + dbn::kMetadataLenLen + mlen) return arrow::Status::OK();

    ARROW_RETURN_NOT_OK(parseMetadata(
        std::span<const std::byte>(p + dbn::kPrefixLen + dbn::kMetadataLenLen, mlen),
        version));
    cursor_ += dbn::kPrefixLen + dbn::kMetadataLenLen + mlen;
    haveMeta_ = true;
  }

  for (;;) {
    const std::size_t avail = pendingBytes();
    if (avail == 0) break;
    const std::byte* p = buf_.data() + cursor_;

    const std::size_t len = static_cast<std::size_t>(u8at(p, 0)) * dbn::kLengthMultiplier;
    if (len < dbn::kRecordHeaderLen) {
      return arrow::Status::Invalid("DBN record length ", len,
                                    " is smaller than the 16-byte record header");
    }
    if (avail < len) break;  // partial record — wait for more bytes

    dbn::RecordHeader hd;
    hd.length       = u8at(p, 0);
    hd.rtype        = u8at(p, 1);
    hd.publisherId  = loadU16(p + 2);
    hd.instrumentId = loadU32(p + 4);
    hd.tsEvent      = loadU64(p + 8);

    if (!dbn::isDeclaredRType(hd.rtype)) {
      return arrow::Status::Invalid(
          "undeclared DBN rtype ", hex2(hd.rtype),
          ". Every rtype DBN declares is accepted (and the ones this adapter "
          "does not consume are skipped and counted); an undeclared one is "
          "refused rather than read as a schema it is not.");
    }
    ++stats_.recordsIn;

    switch (static_cast<dbn::RType>(hd.rtype)) {
      case dbn::RType::Mbo: {
        if (len != dbn::kMboMsgLen) {
          return arrow::Status::Invalid("MboMsg must be ", dbn::kMboMsgLen,
                                        " bytes, record claims ", len);
        }
        dbn::MboMsg m;
        m.hd        = hd;
        m.orderId   = loadU64(p + 16);
        m.price     = loadI64(p + 24);
        m.size      = loadU32(p + 32);
        m.flags     = u8at(p, 36);
        m.channelId = u8at(p, 37);
        m.action    = loadChar(p + 38);
        m.side      = loadChar(p + 39);
        m.tsRecv    = loadU64(p + 40);
        m.tsInDelta = loadI32(p + 48);
        m.sequence  = loadU32(p + 52);
        ARROW_RETURN_NOT_OK(onMbo(m));
        break;
      }
      case dbn::RType::Mbp0: {
        if (len != dbn::kTradeMsgLen) {
          return arrow::Status::Invalid("TradeMsg must be ", dbn::kTradeMsgLen,
                                        " bytes, record claims ", len);
        }
        dbn::TradeMsg m;
        m.hd        = hd;
        m.price     = loadI64(p + 16);
        m.size      = loadU32(p + 24);
        m.action    = loadChar(p + 28);
        m.side      = loadChar(p + 29);
        m.flags     = u8at(p, 30);
        m.depth     = u8at(p, 31);
        m.tsRecv    = loadU64(p + 32);
        m.tsInDelta = loadI32(p + 40);
        m.sequence  = loadU32(p + 44);
        ARROW_RETURN_NOT_OK(onTrade(m));
        break;
      }
      default:
        // A declared rtype this adapter does not consume: the gateway's
        // Error/System/SymbolMapping records, and every schema outside the
        // first slice's plan. Counted, not silently dropped.
        ++stats_.recordsSkipped;
        break;
    }
    cursor_ += len;
  }

  // Always compact: what is left is at most one partial record (< 64 bytes),
  // so the erase is trivial and `buf_` cannot grow across feeds.
  if (cursor_ == buf_.size()) {
    buf_.clear();
    cursor_ = 0;
  } else if (cursor_ > 0) {
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ = 0;
  }
  return arrow::Status::OK();
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

arrow::Status DatabentoAdapter::parseMetadata(std::span<const std::byte> block,
                                              std::uint8_t version) {
  dbn::Metadata md;
  md.version = version;

  Reader r(block);
  md.dataset = r.cstr(dbn::kDatasetCstrLen);
  const std::uint16_t rawSchema = r.u16();
  md.schema = rawSchema == dbn::kNullSchema ? std::optional<std::uint16_t>{}
                                            : std::optional<std::uint16_t>{rawSchema};
  md.start = r.u64();
  md.end   = r.u64();
  md.limit = r.u64();
  if (version == 1) {
    // v1 spent eight bytes on a record_count the later versions reclaimed.
    const std::uint64_t rc = r.u64();
    if (rc != dbn::kNullRecordCount) md.recordCount = rc;
  }
  const std::uint8_t rawStypeIn = r.u8();
  md.stypeIn  = rawStypeIn == dbn::kNullSType ? std::optional<std::uint8_t>{}
                                              : std::optional<std::uint8_t>{rawStypeIn};
  md.stypeOut = r.u8();
  md.tsOut    = r.u8() != 0;
  if (version > 1) {
    md.symbolCstrLen = r.u16();
  } else {
    md.symbolCstrLen = dbn::kSymbolCstrLenV1;
  }
  r.skip(version == 1 ? dbn::kReservedLenV1 : dbn::kReservedLen);
  if (r.bad() || r.offset() != dbn::kFixedMetadataLen) {
    return arrow::Status::Invalid("DBN v", static_cast<int>(version),
                                  " fixed metadata block did not consume exactly ",
                                  dbn::kFixedMetadataLen, " bytes (consumed ",
                                  r.offset(), ")");
  }
  if (md.symbolCstrLen == 0 || md.symbolCstrLen > 1024) {
    return arrow::Status::Invalid("implausible DBN symbol_cstr_len ", md.symbolCstrLen);
  }
  if (md.tsOut) {
    return arrow::Status::Invalid(
        "DBN metadata declares ts_out=1. Records then carry an appended live-gateway "
        "send timestamp, which this adapter does not decode: the first slice ingests "
        "HISTORICAL files (SPEC D6) and no historical file sets it. Refused rather "
        "than read with the wrong record length.");
  }

  const std::uint32_t layoutLen = r.u32();
  if (layoutLen != 0) {
    return arrow::Status::Invalid(
        "DBN metadata carries a schema definition of ", layoutLen,
        " bytes; every version this adapter supports writes 0 there");
  }

  const auto repeated = [&](std::vector<std::string>& out) {
    const std::uint32_t n = r.u32();
    // Bound the count against what is actually left, so a corrupt length
    // cannot make us reserve gigabytes before failing.
    if (!r.bad() && static_cast<std::uint64_t>(n) * md.symbolCstrLen > r.remaining()) {
      out.clear();
      r.skip(r.remaining() + 1);  // latch bad_
      return;
    }
    out.reserve(n);
    for (std::uint32_t i = 0; i < n && !r.bad(); ++i) out.push_back(r.cstr(md.symbolCstrLen));
  };
  repeated(md.symbols);
  repeated(md.partial);
  repeated(md.notFound);

  const std::uint32_t mappingCount = r.u32();
  for (std::uint32_t i = 0; i < mappingCount && !r.bad(); ++i) {
    dbn::SymbolMapping sm;
    sm.rawSymbol = r.cstr(md.symbolCstrLen);
    const std::uint32_t intervals = r.u32();
    const std::size_t each = 2 * sizeof(std::uint32_t) + md.symbolCstrLen;
    if (!r.bad() && static_cast<std::uint64_t>(intervals) * each > r.remaining()) {
      r.skip(r.remaining() + 1);
      break;
    }
    sm.intervals.reserve(intervals);
    for (std::uint32_t j = 0; j < intervals && !r.bad(); ++j) {
      dbn::MappingInterval iv;
      iv.startDate = r.u32();
      iv.endDate   = r.u32();
      iv.symbol    = r.cstr(md.symbolCstrLen);
      sm.intervals.push_back(std::move(iv));
    }
    md.mappings.push_back(std::move(sm));
  }
  if (r.bad()) {
    return arrow::Status::Invalid(
        "DBN metadata is truncated or inconsistent: its variable-length section "
        "ran past the ", block.size(), " bytes the metadata length declared");
  }

  // What is left is v3's 8-byte alignment padding, and it must be zero. A v1 or
  // v2 file has none, so a non-empty tail there is a layout we do not
  // understand rather than padding we may skip.
  md.endPadding = r.remaining();
  if (md.endPadding != 0) {
    if (version < dbn::kFirstAlignedVersion ||
        md.endPadding >= dbn::kMetadataAlignment) {
      return arrow::Status::Invalid(
          "DBN v", static_cast<int>(version), " metadata has ", md.endPadding,
          " unconsumed trailing byte(s); only v", static_cast<int>(dbn::kFirstAlignedVersion),
          "+ pads, and only by 1..", dbn::kMetadataAlignment - 1);
    }
    for (std::size_t i = 0; i < md.endPadding; ++i) {
      if (u8at(block.data() + r.offset(), i) != 0) {
        return arrow::Status::Invalid(
            "DBN v", static_cast<int>(version),
            " metadata alignment padding is not zero at byte ", i);
      }
    }
  }

  // ---- venue ------------------------------------------------------------
  if (venueId_ == kInvalidSymbolId) {
    if (md.dataset.empty()) {
      return arrow::Status::Invalid(
          "DBN metadata carries an empty dataset and Config::venue is unset, so "
          "there is no venue label for these rows. An empty venue in L1 is "
          "indistinguishable from a provider gap.");
    }
    venueId_ = venues_.intern(md.dataset);
  }

  // ---- symbology (SPEC D7, docs/data-layer.md §8.9) ---------------------
  //
  // The mappings are recorded in whichever direction the request used, so both
  // are handled rather than one being assumed:
  //
  //   stype_out == InstrumentId  ->  rawSymbol is the TICKER and each
  //                                  interval's symbol is the numeric id.
  //                                  (This is what the fixtures carry:
  //                                  ESH1 -> [20201228, 20201229) = "5482".)
  //   stype_in  == InstrumentId  ->  the other way round.
  //
  // Anything else leaves instrument_id unmentioned, and then the only honest
  // answer is to require Config::symbolOverride instead of inventing a ticker.
  const bool outIsId = md.stypeOut == static_cast<std::uint8_t>(dbn::SType::InstrumentId);
  const bool inIsId  = md.stypeIn.has_value() &&
                       *md.stypeIn == static_cast<std::uint8_t>(dbn::SType::InstrumentId);
  for (const auto& sm : md.mappings) {
    if (outIsId) {
      const SymbolId ticker = symbols_.intern(sm.rawSymbol);
      for (const auto& iv : sm.intervals) {
        std::uint32_t id = 0;
        if (!parseU32(iv.symbol, id)) continue;  // not an id in this interval
        symbology_[id].push_back(SymbolWindow{iv.startDate, iv.endDate, ticker});
      }
    } else if (inIsId) {
      std::uint32_t id = 0;
      if (!parseU32(sm.rawSymbol, id)) continue;
      for (const auto& iv : sm.intervals) {
        symbology_[id].push_back(
            SymbolWindow{iv.startDate, iv.endDate, symbols_.intern(iv.symbol)});
      }
    }
  }
  if (symbology_.empty() && overrideSymbolId_ == kInvalidSymbolId) {
    return arrow::Status::Invalid(
        "DBN metadata gives no instrument_id -> ticker mapping (stype_in=",
        md.stypeIn.has_value() ? static_cast<int>(*md.stypeIn) : -1,
        ", stype_out=", static_cast<int>(md.stypeOut), ", ", md.mappings.size(),
        " mapping(s)), and Config::symbolOverride is unset. `symbol` is L1's "
        "leading key column (SPEC D7) and must not be guessed.");
  }

  meta_ = std::move(md);
  return arrow::Status::OK();
}

// ---------------------------------------------------------------------------
// Header mapping
// ---------------------------------------------------------------------------

arrow::Result<RecordHeader> DatabentoAdapter::makeHeader(const dbn::RecordHeader& hd,
                                                         std::uint64_t tsRecv,
                                                         std::uint32_t sequence) {
  if (!publisherId_.has_value()) {
    publisherId_ = hd.publisherId;
  } else if (*publisherId_ != hd.publisherId) {
    return arrow::Status::Invalid(
        "DBN publisher_id changed mid-stream (", *publisherId_, " -> ", hd.publisherId,
        "). Every row in this batch would carry the same `venue` label, and one of "
        "them would be wrong. Decode one publisher per adapter.");
  }

  if (hd.tsEvent == dbn::kUndefTimestamp) {
    return arrow::Status::Invalid(
        "DBN record carries ts_event = kUndefTimestamp. `ts_event` is the ONLY time "
        "an operator may key off (SPEC D9); an undefined one must not reach L1.");
  }
  if (hd.tsEvent > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return arrow::Status::Invalid("DBN ts_event ", hd.tsEvent,
                                  " does not fit the signed nanosecond column");
  }

  RecordHeader h;
  h.tsEventNs = static_cast<std::int64_t>(hd.tsEvent);
  // ts_recv is DIAGNOSTICS ONLY and never a window boundary, so an undefined
  // one becomes 0 rather than failing the row.
  h.tsRecvNs =
      (tsRecv == dbn::kUndefTimestamp ||
       tsRecv > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
          ? 0
          : static_cast<std::int64_t>(tsRecv);
  // SPEC D6: FROM DATABENTO, NEVER SYNTHESIZED. The venue sequence is stable
  // across a re-download, which is what makes re-ingest converge and the
  // splice's dedup exact.
  h.providerSeq   = sequence;
  h.sessionId     = cfg_.sessionId;
  h.venueId       = venueId_;
  h.instrumentId  = hd.instrumentId;
  ARROW_ASSIGN_OR_RAISE(h.symbolId, resolveSymbol(hd.instrumentId, h.tsEventNs));
  return h;
}

arrow::Result<SymbolId> DatabentoAdapter::resolveSymbol(std::uint32_t instrumentId,
                                                        std::uint64_t tsEventNs) {
  if (overrideSymbolId_ != kInvalidSymbolId) return overrideSymbolId_;
  const auto it = symbology_.find(instrumentId);
  if (it == symbology_.end()) {
    return arrow::Status::Invalid(
        "no symbology entry for instrument_id ", instrumentId,
        ". The DBN metadata names ", symbology_.size(),
        " instrument(s); a record for one it does not name cannot be given a ticker, "
        "and `symbol` is L1's leading key column (SPEC D7).");
  }
  const std::uint32_t day = yyyymmddFromNs(static_cast<std::int64_t>(tsEventNs));
  for (const auto& w : it->second) {
    if (day >= w.startDate && day < w.endDate) return w.id;  // half-open
  }
  return arrow::Status::Invalid(
      "instrument_id ", instrumentId, " has ", it->second.size(),
      " symbology window(s), none covering ", day,
      ". A ticker reused across a delisting is exactly what these windows exist to "
      "detect (docs/data-layer.md §8.9), so this is refused rather than resolved to "
      "the nearest one.");
}

// ---------------------------------------------------------------------------
// mbo -> BookRecord
// ---------------------------------------------------------------------------

arrow::Status DatabentoAdapter::onMbo(const dbn::MboMsg& m) {
  if (!dbn::isDeclaredAction(m.action)) {
    return arrow::Status::Invalid("undeclared DBN action ", showChar(m.action),
                                  " on an MboMsg (declared: A C M T F R N)");
  }
  if (!dbn::isDeclaredSide(m.side)) {
    return arrow::Status::Invalid("undeclared DBN side ", showChar(m.side),
                                  " on an MboMsg (declared: A B N)");
  }

  ARROW_ASSIGN_OR_RAISE(const RecordHeader h, makeHeader(m.hd, m.tsRecv, m.sequence));

  // SPEC D8: an assignment, not a conversion. DBN is int64 at 1e-9 and so are
  // we; the only translation is the sentinel, INT64_MAX -> INT64_MIN.
  std::int64_t px = m.price;
  if (m.price == dbn::kUndefPrice) {
    px = kNoPrice;
    ++stats_.undefPrice;
  }
  std::uint64_t sz = m.size;
  if (m.size == dbn::kUndefOrderSize) {
    sz = 0;
    ++stats_.undefSize;
  }

  std::uint16_t flags = m.flags;
  const auto side = static_cast<dbn::Side>(m.side);
  const bool isPrint = m.action == static_cast<char>(dbn::Action::Trade) ||
                       m.action == static_cast<char>(dbn::Action::Fill);

  // `BookRecord::side`, and this is the subtle one.
  //
  // On Add/Cancel/Modify/Clear it is the RESTING order's side, which is what
  // DBN reports and what `OrderBookManager::onAdd/onUpdate` want.
  //
  // On a `BookAction::Trade` row it is the AGGRESSOR's side, because that row
  // drives `OrderBookManager::onTrade(..., Aggressor)`. DBN's own definition
  // of `side` is "the side of the market for resting orders, OR the side of
  // the aggressor for trades" — so a `Trade` record already reports the
  // aggressor, while a `Fill` reports the RESTING order being filled and must
  // be FLIPPED. `ItchAdapter::routeOrderExecuted` does exactly this flip
  // ("The resting order's side is passive; the aggressor is the opposite",
  // ItchAdapter.cpp:180-181), and not doing it would put two different
  // meanings in one column.
  Side out = Side::Bid;
  bool sideKnown = true;
  switch (side) {
    case dbn::Side::Bid: out = Side::Bid; break;
    case dbn::Side::Ask: out = Side::Ask; break;
    case dbn::Side::None:
      // No representation in data::Side — see kGmaFlagSideUnspecified.
      sideKnown = false;
      flags |= kGmaFlagSideUnspecified;
      ++stats_.sideUnspecified;
      break;
  }
  if (sideKnown && m.action == static_cast<char>(dbn::Action::Fill)) {
    out = out == Side::Bid ? Side::Ask : Side::Bid;
  }

  const OrderKey key{m.hd.instrumentId, m.orderId};

  const auto emit = [&](BookAction action, std::int64_t price, std::uint64_t size,
                        std::uint32_t epoch) {
    BookRecord r;
    r.header   = h;
    r.action   = action;
    r.side     = out;
    r.price    = price;
    r.size     = size;
    r.orderId  = m.orderId;
    // Order::priority is "recv seq or venue ts". DBN's per-channel `sequence`
    // is the arrival order within a level, which is what queue position means.
    // (ItchAdapter passes 0 because the ITCH JSON carries no sequence.)
    r.priority = m.sequence;
    r.depth    = 0;  // mbo is per-order (L3); depth is for aggregated feeds
    r.epoch    = epoch;
    r.flags    = flags;
    book_.push_back(r);
    ++stats_.bookEventsOut;
  };

  // Resolve a delta-quantity removal against the tracked resting size, exactly
  // as ItchAdapter::routeOrderCancel does: `removed >= remaining` is a delete,
  // anything less is an update to the remainder. Returns nothing; emits.
  const auto applyRemoval = [&](std::uint64_t removed, std::uint32_t epoch) {
    const auto it = orders_.find(key);
    if (it == orders_.end()) {
      // We never saw this order's Add — it rests from before our window, or
      // from before a Clear. Nothing authoritative to subtract from, so the
      // removal is recorded as a full delete and counted. ItchAdapter DROPS
      // this case outright (it cannot even name the symbol); keeping the row
      // preserves the event, and `OrderBookManager::onDelete` on an unknown
      // key is a no-op on replay.
      ++stats_.unknownOrder;
      emit(BookAction::Delete, px, removed, epoch);
      return;
    }
    if (removed >= it->second) {
      orders_.erase(it);
      // `size` on a Delete row is the quantity REMOVED, not a remaining size:
      // a deleted order has no remainder, and writing 0 would discard the only
      // quantity the message carries. Harmless on replay — onDelete takes no
      // size.
      emit(BookAction::Delete, px, removed, epoch);
    } else {
      it->second -= removed;
      emit(BookAction::Update, px, it->second, epoch);
    }
  };

  switch (static_cast<dbn::Action>(m.action)) {
    case dbn::Action::Clear: {
      // The Clear row carries the NEW epoch, matching
      // OrderBookManager::onReset(symbol, newEpoch); OrderKey carries the
      // epoch so a pre-reset order cannot be matched by a post-reset message.
      const std::uint32_t next = epochOf(m.hd.instrumentId) + 1;
      epochs_[m.hd.instrumentId] = next;
      ++stats_.clears;
      for (auto it = orders_.begin(); it != orders_.end();) {
        it = it->first.instrumentId == m.hd.instrumentId ? orders_.erase(it)
                                                         : std::next(it);
      }
      emit(BookAction::Clear, px, sz, next);
      break;
    }

    case dbn::Action::Add:
      trackOrder(key, sz);
      emit(BookAction::Add, px, sz, epochOf(m.hd.instrumentId));
      break;

    case dbn::Action::Modify:
      // DBN keeps the SAME order_id and carries the new absolute price and
      // size, so this is one Update. ITCH's order_replace issues a NEW order
      // ref, which is why ItchAdapter emits delete+add there — a wire
      // difference, not a semantic one.
      //
      // ItchAdapter returns early when the original is untracked (it cannot
      // resolve the symbol without it); here the record is self-sufficient, so
      // the Update is emitted and the miss is counted.
      if (orders_.find(key) == orders_.end()) ++stats_.unknownOrder;
      trackOrder(key, sz);
      emit(BookAction::Update, px, sz, epochOf(m.hd.instrumentId));
      break;

    case dbn::Action::Cancel:
      applyRemoval(sz, epochOf(m.hd.instrumentId));
      break;

    case dbn::Action::Fill:
      // DBN: "An existing order was filled. Does not affect the book." The
      // resting order's reduction arrives separately as a Cancel or Modify, so
      // by default only the print is emitted. Config::fillReducesRestingOrder
      // takes ItchAdapter's branch instead (book mutation FIRST, then the
      // print — ItchAdapter.cpp:188-199) for a dataset measured to need it.
      if (cfg_.fillReducesRestingOrder) applyRemoval(sz, epochOf(m.hd.instrumentId));
      emit(BookAction::Trade, px, sz, epochOf(m.hd.instrumentId));
      break;

    case dbn::Action::Trade:
      // DBN: "An aggressing order traded. Does not affect the book." Identical
      // to ItchAdapter's top-level `trade` print (M1): emitted print-only.
      emit(BookAction::Trade, px, sz, epochOf(m.hd.instrumentId));
      break;

    case dbn::Action::None:
      // DBN: "Has no effect on the book, but may carry flags or other
      // information." ACCEPTED (not refused) and deliberately produces no
      // book row, because there is no BookAction for "no effect" and inventing
      // one would put a no-op mutation into the replay stream.
      ++stats_.actionNone;
      break;
  }
  return arrow::Status::OK();
}

// ---------------------------------------------------------------------------
// trades -> TickRecord
// ---------------------------------------------------------------------------

arrow::Status DatabentoAdapter::onTrade(const dbn::TradeMsg& m) {
  if (!dbn::isDeclaredAction(m.action)) {
    return arrow::Status::Invalid("undeclared DBN action ", showChar(m.action),
                                  " on a TradeMsg (declared: A C M T F R N)");
  }
  if (!dbn::isDeclaredSide(m.side)) {
    return arrow::Status::Invalid("undeclared DBN side ", showChar(m.side),
                                  " on a TradeMsg (declared: A B N)");
  }

  ARROW_ASSIGN_OR_RAISE(const RecordHeader h, makeHeader(m.hd, m.tsRecv, m.sequence));

  TickRecord t;
  t.header = h;
  t.kind   = RecordKind::Trade;
  t.price  = m.price;
  if (m.price == dbn::kUndefPrice) {
    t.price = kNoPrice;
    ++stats_.undefPrice;
  }
  t.size = m.size;
  if (m.size == dbn::kUndefOrderSize) {
    t.size = 0;
    ++stats_.undefSize;
  }

  // A `trades` record reports the AGGRESSOR's side directly (no flip — this is
  // not a resting order). ItchAdapter's top-level `trade` does the same, with
  // ITCH's 'B'/'S' spelling where DBN uses 'B'/'A'.
  switch (static_cast<dbn::Side>(m.side)) {
    case dbn::Side::Bid: t.aggressor = Aggressor::Buy; break;
    case dbn::Side::Ask: t.aggressor = Aggressor::Sell; break;
    case dbn::Side::None:
      // TickRecord HAS a representation for this, unlike BookRecord.
      t.aggressor = Aggressor::Unknown;
      ++stats_.sideUnspecified;
      break;
  }

  // ALWAYS TRUE, and it is SPEC D12 / docs C13 rather than a default.
  //
  // The same execution is in the store twice — once here and once as MBO
  // trade/fill records — so a `trades` row replayed into the book would
  // double-count volume against the mutations that arrived as add/update/
  // delete. ItchAdapter sets this on every ObTradeEvent it emits, at both
  // sites, and there is no site in this repo that emits one with it clear.
  t.bookNeutral = true;

  // DBN's TradeMsg carries no trade id; `depth` is 0 for the trades schema
  // (a non-zero one would be an mbp record, which the rtype already
  // discriminates), and `channel_id` / `ts_in_delta` are gateway diagnostics
  // with no column. Dropped deliberately, named so the drop is not a surprise.
  t.tradeId = 0;
  t.flags   = m.flags;

  ticks_.push_back(t);
  ++stats_.ticksOut;
  return arrow::Status::OK();
}

}  // namespace gma::data
