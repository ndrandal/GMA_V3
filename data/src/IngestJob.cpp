//
// IngestJob — see IngestJob.hpp for the path and what is deliberately not here.
//

#include "gma/data/IngestJob.hpp"

#include "gma/data/DatabentoAdapter.hpp"
#include "gma/data/SymbolTable.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace gma::data::ingest {

// ===========================================================================
// THE TWO RULINGS THIS TICKET OWED. Text, so that the tool's output, the Linear
// comment and SPEC §5 cannot drift into three different answers.
// ===========================================================================

const char* const kSessionIdRuling =
    "A RE-INGEST REUSES ITS session_id, AND session_id IS DERIVED FROM THE "
    "PULL RATHER THAN ALLOCATED PER RUN.\n"
    "  Decided here because the alternative breaks two locked decisions. D6 "
    "makes L1 'a derived, rebuildable projection of the raw DBN files'; a "
    "projection whose output changes when you rebuild it from unchanged input "
    "has a hidden input, and a per-run counter is exactly that. D5's re-ingest "
    "('DROP PARTITION + reload reproduces identical rows') and ENC-1487's "
    "bit-identical assertions both fail on the session_id column alone if a "
    "reload renumbers it. And DatabentoAdapter's own note on provider_seq — "
    "'FROM DATABENTO, NEVER SYNTHESIZED ... stable across a re-download, which "
    "is what makes re-ingest converge and the splice's dedup exact' — is about "
    "the PAIR (session_id, provider_seq) (docs §8.3); a fresh session_id each "
    "run makes the stable half useless.\n"
    "  So session_id = FNV-1a-32(dataset | schema | vendor job_id), forced "
    "non-zero. Same retained file, same rows, forever; a NEW PULL of the same "
    "window gets a new job_id and therefore a new session, which is the one "
    "case where the distinction is real.\n"
    "  AND A CORRECTION TO docs/data-layer.md §8.8, WHICH THE RULING DOES NOT "
    "RESCUE: §8.8 calls (symbol, kind, from_ts, session_id) 'unique by "
    "construction'. THAT IS FALSE UNDER BOTH ANSWERS, not just under reuse. "
    "ENC-1480 made the table an append-only transition log, so one window and "
    "one session write at least two rows (state=0 then state=1) and a failed "
    "run writes three. The claim dates from revision 2's one-row-per-window "
    "ReplacingMergeTree and was voided by the rewrite that replaced it. What "
    "makes the table readable is the READ RULE, never uniqueness.";

const char* const kGapsNote =
    "ZERO gma.gaps ROWS WRITTEN, AND NOT FOR LACK OF SCOPE: THE SEQ-"
    "DISCONTINUITY DETECTOR CANNOT RUN ON A SYMBOL-FILTERED PULL.\n"
    "  docs §8.8 detects a provider hole as 'a seq discontinuity'. Databento's "
    "`sequence` is the VENUE's, not the symbol's: in this sample consecutive "
    "MSFT records step 11404021 -> 11404028 -> 11423666, because every other "
    "Nasdaq instrument's messages occupy the numbers in between. provider_seq "
    "is MONOTONIC within (session_id, symbol) exactly as §8.3 says — measured "
    "sorted, 1000/1000, both kinds — but it is not CONTIGUOUS, and gap "
    "detection needs contiguity. Writing a gap row per jump would have "
    "produced ~900 phantom holes per kind, each of which a research run joining "
    "`gaps` would read as 'there was no data here': the same wrong-answer class "
    "the table exists to prevent, manufactured by the detector. A real detector "
    "for a filtered pull needs either an unfiltered channel feed or the "
    "vendor's own condition.json (which for 2022-06-10 reads 'available'), and "
    "that is a ticket, not a line in this one.";

namespace {

// ---------------------------------------------------------------------------
// session_id derivation — see kSessionIdRuling.
// ---------------------------------------------------------------------------
std::uint32_t deriveSessionId(const std::string& dataset, const std::string& schema,
                              const std::string& jobId) {
  // FNV-1a 32. Chosen for being a fixed, written-down function of the input
  // rather than for its distribution: the property that matters is that the
  // same pull yields the same number on every machine and every rebuild.
  std::uint32_t h = 0x811C9DC5u;
  const auto mix = [&h](std::string_view s) {
    for (const char c : s) {
      h ^= static_cast<std::uint8_t>(c);
      h *= 0x01000193u;
    }
    h ^= static_cast<std::uint8_t>('|');
    h *= 0x01000193u;
  };
  mix(dataset);
  mix(schema);
  mix(jobId);
  // 0 is reserved: a zero session_id on an event row is indistinguishable
  // from a default-constructed header.
  return h == 0 ? 1u : h;
}

std::string expandTilde(const std::string& p) {
  if (p.empty() || p[0] != '~') return p;
  const char* home = std::getenv("HOME");
  if (home == nullptr) return p;
  return std::string(home) + p.substr(1);
}

bool fileExists(const std::string& p) {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

// ---------------------------------------------------------------------------
// Decode a DBN buffer through the adapter, in `chunk`-byte feeds.
//
// The chunk size is a parameter because it is the round-trip's second axis:
// the adapter buffers across `feed()` calls, so a chunk that straddles record
// and metadata boundaries drives a different code path through the same
// framing than one whole-file feed. Two chunkings that produce identical
// canonical records is a real property; one chunking repeated is not.
// ---------------------------------------------------------------------------
struct Decoded {
  std::vector<TickRecord> ticks;
  std::vector<BookRecord> book;
  DatabentoAdapter::Stats stats;
  dbn::Metadata meta;
  std::string symbol;
  std::string venue;
  std::uint32_t instrumentId = 0;
};

arrow::Result<Decoded> decodeDbn(const std::vector<std::byte>& bytes,
                                 std::uint32_t sessionId, std::size_t chunk) {
  SymbolTable symbols;
  SymbolTable venues;
  DatabentoAdapter::Config cfg;
  cfg.sessionId = sessionId;
  // `venue` empty => the adapter interns the DBN metadata's `dataset`
  // ("XNAS.ITCH"), which is what makes the column checkable by hand in
  // ClickHouse. `symbolOverride` empty => the ticker is RESOLVED from the
  // metadata's symbology (SPEC D7) rather than asserted by us, which is the
  // point of writing the symbology section into the DBN at all.
  //
  // `fillReducesRestingOrder` IS LEFT AT ITS DEFAULT, AND THE DEFAULT IS NOW
  // MEASURED: see the comment in DatabentoAdapter.hpp. 91 of 91 `F` records in
  // this sample are accompanied by a `C` for the SAME order_id at the same
  // (ts_event, sequence), so the venue emits the resting reduction itself and
  // applying the fill as a second reduction double-reduces on 100% of fills.
  DatabentoAdapter adapter(cfg, symbols, venues);

  for (std::size_t off = 0; off < bytes.size(); off += chunk) {
    const std::size_t n = std::min(chunk, bytes.size() - off);
    ARROW_RETURN_NOT_OK(adapter.feed(std::span<const std::byte>(bytes.data() + off, n)));
  }
  ARROW_RETURN_NOT_OK(adapter.finish());

  Decoded d;
  d.ticks = adapter.takeTrades();
  d.book  = adapter.takeBookEvents();
  d.stats = adapter.stats();
  if (!adapter.metadata().has_value()) {
    return arrow::Status::Invalid("adapter produced no metadata");
  }
  d.meta = *adapter.metadata();

  // The symbol and venue the adapter RESOLVED, read back out of the interning
  // tables rather than assumed. A single symbol is this slice's scope (SPEC
  // §6: one symbol), and more than one is refused rather than averaged.
  const auto* records = !d.book.empty() ? nullptr : nullptr;
  (void)records;
  std::set<std::uint32_t> symbolIds;
  std::set<std::uint32_t> instrumentIds;
  std::uint32_t venueId = 0;
  const auto noteHeader = [&](const RecordHeader& h) {
    symbolIds.insert(h.symbolId);
    instrumentIds.insert(h.instrumentId);
    venueId = h.venueId;
  };
  for (const auto& t : d.ticks) noteHeader(t.header);
  for (const auto& b : d.book) noteHeader(b.header);
  if (symbolIds.size() != 1 || instrumentIds.size() != 1) {
    return arrow::Status::Invalid(
        "decode yielded ", symbolIds.size(), " symbol(s) and ",
        instrumentIds.size(),
        " instrument_id(s). This slice is one symbol (SPEC §6) and the window "
        "is recorded per symbol in gma.ingest_windows, so a multi-symbol file "
        "is refused rather than recorded under one of them.");
  }
  d.symbol       = symbols.name(*symbolIds.begin());
  d.venue        = venues.name(venueId);
  d.instrumentId = *instrumentIds.begin();
  return d;
}

// ---------------------------------------------------------------------------
// The sort key, and sorting by it before insert.
//
// docs §8.6: "Insert symbol-sorted batches per day partition — the key leads
// with symbol, so raw DBN file order (time-ordered) forces a per-part re-sort.
// The ingest job owns this; a background merge cannot fix it after the fact."
// At one symbol and one day this sort is the identity, which is exactly why it
// is written now: the cost of getting it wrong is invisible here and expensive
// at the universe.
// ---------------------------------------------------------------------------
struct SortKey {
  std::string   symbol;
  std::int64_t  tsEventNs;
  std::uint64_t providerSeq;
  bool operator<(const SortKey& o) const {
    if (symbol != o.symbol) return symbol < o.symbol;
    if (tsEventNs != o.tsEventNs) return tsEventNs < o.tsEventNs;
    return providerSeq < o.providerSeq;
  }
  bool operator==(const SortKey& o) const = default;
};

std::uint32_t dayPartitionOf(std::int64_t tsNs) {
  return csv::yyyymmddUtc(static_cast<std::uint64_t>(tsNs));
}

// ---------------------------------------------------------------------------
// Row formatting. Columns are NAMED in the INSERT so a DDL column reorder is a
// server-side error rather than a silently transposed table.
// ---------------------------------------------------------------------------
constexpr const char* kTradesColumns =
    "ts_event, ts_recv, provider_seq, session_id, symbol, instrument_id, venue, "
    "price, size, aggressor, book_neutral, trade_id, flags";

constexpr const char* kBookEventsColumns =
    "ts_event, ts_recv, provider_seq, session_id, symbol, instrument_id, venue, "
    "action, side, price, size, order_id, priority, depth, epoch, flags";

void appendTradeRow(std::ostringstream& os, const TickRecord& t,
                    const std::string& symbol, const std::string& venue) {
  os << t.header.tsEventNs << '\t' << t.header.tsRecvNs << '\t'
     << t.header.providerSeq << '\t' << t.header.sessionId << '\t'
     << ch::tsvEscape(symbol) << '\t' << t.header.instrumentId << '\t'
     << ch::tsvEscape(venue) << '\t' << t.price << '\t' << t.size << '\t'
     << static_cast<unsigned>(t.aggressor) << '\t' << (t.bookNeutral ? 1 : 0) << '\t'
     << t.tradeId << '\t' << t.flags << '\n';
}

void appendBookRow(std::ostringstream& os, const BookRecord& b,
                   const std::string& symbol, const std::string& venue) {
  os << b.header.tsEventNs << '\t' << b.header.tsRecvNs << '\t'
     << b.header.providerSeq << '\t' << b.header.sessionId << '\t'
     << ch::tsvEscape(symbol) << '\t' << b.header.instrumentId << '\t'
     << ch::tsvEscape(venue) << '\t' << static_cast<unsigned>(b.action) << '\t'
     << static_cast<unsigned>(b.side) << '\t' << b.price << '\t' << b.size << '\t'
     << b.orderId << '\t' << b.priority << '\t' << b.depth << '\t' << b.epoch << '\t'
     << b.flags << '\n';
}

// ---------------------------------------------------------------------------
// ingest_windows writes. updated_at IS STAMPED EXPLICITLY ON EVERY ROW,
// including the state=0 one. The DDL's own note: with no value the column
// defaults to epoch 0, every row ties, "the single newest row" becomes
// arbitrary and a stale state=1 can win — the identical zero-valued-clock
// defect the table was rewritten to eliminate. The DEFAULT now64(3) in the DDL
// is the backstop; writing it is the contract.
// ---------------------------------------------------------------------------
arrow::Status writeWindowRow(const ch::Client& client, const std::string& symbol,
                             const std::string& kind, std::int64_t fromNs,
                             std::int64_t toNs, std::uint8_t state,
                             std::uint64_t rowsInserted, std::uint32_t sessionId,
                             const std::string& sourceRef) {
  std::ostringstream os;
  os << "INSERT INTO gma.ingest_windows (symbol, kind, from_ts, to_ts, state, "
        "rows_inserted, updated_at, completed_at, session_id, source, source_ref) "
        "FORMAT TabSeparated\n";
  os << ch::tsvEscape(symbol) << '\t' << ch::tsvEscape(kind) << '\t' << fromNs << '\t'
     << toNs << '\t' << static_cast<unsigned>(state) << '\t' << rowsInserted << '\t'
     // updated_at: a WRITE clock, in ms, stamped now, on every state.
     << "now64(3)" << '\t'
     // completed_at: a COMPLETION clock. Zero unless this row IS the
     // completion — the DDL is explicit that it is zero while in flight and
     // that ordering by it is what made the revision-2 engine delete the
     // in-flight row.
     << (state == 1 ? "now64(3)" : "0") << '\t' << sessionId << '\t' << "databento"
     << '\t' << ch::tsvEscape(sourceRef) << '\n';
  // `now64(3)` is a FUNCTION and TabSeparated would store it as a string, so
  // the two clock columns go through a SELECT instead. Same row, same
  // transaction-less insert; the only difference is that the server evaluates
  // the clock.
  std::ostringstream q;
  q << "INSERT INTO gma.ingest_windows (symbol, kind, from_ts, to_ts, state, "
       "rows_inserted, updated_at, completed_at, session_id, source, source_ref) "
       "SELECT '"
    << symbol << "', '" << kind << "', toDateTime64(" << fromNs
    << "/1000000000, 9, 'UTC'), toDateTime64(" << toNs << "/1000000000, 9, 'UTC'), "
    << static_cast<unsigned>(state) << ", " << rowsInserted << ", now64(3), "
    << (state == 1 ? "now64(3)" : "toDateTime64(0, 3, 'UTC')") << ", " << sessionId
    << ", 'databento', '" << sourceRef << "'";
  return client.exec(q.str()).status();
}

std::string isoFromNs(std::int64_t ns) {
  const auto day = csv::yyyymmddUtc(static_cast<std::uint64_t>(ns));
  const std::int64_t midnight =
      static_cast<std::int64_t>(csv::midnightUtcNs(day));
  std::int64_t rem = ns - midnight;
  const int h = static_cast<int>(rem / 3600000000000LL);
  rem %= 3600000000000LL;
  const int m = static_cast<int>(rem / 60000000000LL);
  rem %= 60000000000LL;
  const int s = static_cast<int>(rem / 1000000000LL);
  const long long frac = rem % 1000000000LL;
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02d:%02d:%02d.%09lldZ",
                day / 10000, (day / 100) % 100, day % 100, h, m, s, frac);
  return buf;
}

}  // namespace

std::string formatNs(std::int64_t ns) { return isoFromNs(ns); }

std::string formatWindow(std::int64_t fromNs, std::int64_t toNs) {
  return "[" + isoFromNs(fromNs) + ", " + isoFromNs(toNs) + ")";
}

// ===========================================================================
// The completeness read (D11)
// ===========================================================================

arrow::Result<Completeness> queryCompleteness(const ch::Client& client,
                                              const std::string& symbol,
                                              const std::string& kind,
                                              std::int64_t reqFromNs,
                                              std::int64_t reqToNs) {
  // Projection + a WHERE on the sort-key prefix. No aggregate, no FINAL, no
  // argMax, no LIMIT: the resolution and the coverage test are both done by
  // the caller, which is the DDL's own instruction and keeps this a plain
  // selection on this side of the D2 boundary.
  std::ostringstream q;
  q << "SELECT toUnixTimestamp64Nano(from_ts), toUnixTimestamp64Nano(to_ts), "
       "toUInt16(state), toUnixTimestamp64Milli(updated_at) "
       "FROM gma.ingest_windows WHERE symbol = '"
    << symbol << "' AND kind = '" << kind << "' FORMAT TabSeparated";
  ARROW_ASSIGN_OR_RAISE(const auto rows, client.select(q.str()));

  std::vector<IngestWindowRow> parsed;
  parsed.reserve(rows.size());
  for (const auto& r : rows) {
    if (r.size() != 4) {
      return arrow::Status::Invalid("gma.ingest_windows row has ", r.size(),
                                    " field(s), expected 4");
    }
    IngestWindowRow w;
    w.fromTsNs    = std::stoll(r[0]);
    w.toTsNs      = std::stoll(r[1]);
    w.state       = static_cast<std::uint8_t>(std::stoul(r[2]));
    w.updatedAtMs = std::stoll(r[3]);
    parsed.push_back(w);
  }
  return isComplete(parsed, reqFromNs, reqToNs);
}

arrow::Result<std::int64_t> rowCount(const ch::Client& client,
                                     const std::string& table,
                                     const std::string& symbol,
                                     std::int64_t fromNs, std::int64_t toNs) {
  std::ostringstream q;
  q << "SELECT count() FROM gma." << table << " WHERE symbol = '" << symbol
    << "' AND ts_event >= toDateTime64(" << fromNs
    << "/1000000000, 9, 'UTC') AND ts_event < toDateTime64(" << toNs
    << "/1000000000, 9, 'UTC')";
  ARROW_ASSIGN_OR_RAISE(const auto s, client.scalar(q.str()));
  return std::stoll(s);
}

// ===========================================================================
// run
// ===========================================================================

arrow::Result<Report> run(const Options& opts) {
  Report rep;
  rep.sessionIdRuling = kSessionIdRuling;
  rep.gapsNote        = kGapsNote;
  ARROW_ASSIGN_OR_RAISE(rep.target, ch::resolveTarget(opts.url));
  const ch::Client client(rep.target);

  const std::string root = expandTilde(opts.dataRoot);

  // The two kinds. `kind` is gma.ingest_windows' value (and the table name);
  // `dbnSchema` is the vendor's. They differ for MBO — the vendor calls the
  // schema `mbo` and L1's table is `book_events` — and conflating them is how
  // a completeness row gets written under a kind no reader queries.
  struct KindSpec {
    const char* kind;
    const char* dbnSchema;
    const char* table;
  };
  const KindSpec specs[] = {
      KindSpec{"trades", "trades", "trades"},
      KindSpec{"book_events", "mbo", "book_events"},
  };

  // ---- pass 1: the raw half (transcode, retain, decode, round-trip) ------
  //
  // Every file is turned into canonical rows BEFORE anything is dropped or
  // inserted. A `DROP PARTITION` followed by a decode failure would leave the
  // partition empty with no row able to say so beyond a state=0; doing the
  // whole raw half first means a bad file is refused with L1 untouched.
  struct Staged {
    KindReport rep;
    std::vector<TickRecord> ticks;
    std::vector<BookRecord> book;
    std::vector<csv::Transcoded::Mapping> mappings;
    std::string table;
    std::string dataset;
  };
  std::vector<Staged> staged;

  for (const auto& spec : specs) {
    Staged st;
    st.rep.kind      = spec.kind;
    st.rep.dbnSchema = spec.dbnSchema;
    st.table         = spec.table;

    const std::string dbnPath =
        root + "/xnas-itch-20220610." + spec.dbnSchema + ".dbn";
    st.rep.dbnPath = dbnPath;

    std::vector<std::byte> dbnBytes;
    csv::PullMetadata md;

    if (opts.fromDbnOnly) {
      // THE PURE D6 RELOAD. No CSV, no metadata.json: the retained DBN carries
      // `start`, `end` and `limit` in its own metadata block, which is exactly
      // what makes "reload must reload from something" a complete statement.
      if (!fileExists(dbnPath)) {
        return arrow::Status::IOError(
            "--from-dbn-only but ", dbnPath,
            " does not exist. SPEC D6 retains the raw DBN so a reload needs no "
            "re-pull; run without the flag once to produce it from the CSV.");
      }
      ARROW_ASSIGN_OR_RAISE(dbnBytes, csv::readFile(dbnPath));
      st.rep.dbnExisted = true;
    } else {
      // metadata.json: PER JOB, and this directory holds two jobs.
      //
      // Each vendor zip carries its own metadata.json, and extracting both
      // zips into one directory leaves ONE of them — silently the wrong one
      // for one of the two CSVs. So a sidecar is preferred and a shared
      // metadata.json is accepted ONLY if its own `query.schema` matches the
      // file being read. A mismatch is refused with the command that fixes it,
      // because the fields at stake (`start`, `end`, `limit`) are what
      // gma.ingest_windows has an opinion about.
      const std::string sidecar =
          root + "/metadata." + spec.dbnSchema + ".json";
      const std::string shared = root + "/metadata.json";
      std::string mdPath = fileExists(sidecar) ? sidecar : shared;
      ARROW_ASSIGN_OR_RAISE(md, csv::readPullMetadata(mdPath));
      if (md.schema != spec.dbnSchema) {
        return arrow::Status::Invalid(
            mdPath, " describes the `", md.schema, "` job (job_id ", md.jobId,
            ") but the `", spec.dbnSchema,
            "` CSV is being read. Two batch jobs extracted into one directory "
            "leave one metadata.json, and `start`/`end`/`limit` from the wrong "
            "job would be recorded in gma.ingest_windows as this pull's "
            "window. Extract the per-job sidecar:\n"
            "    for z in ", root, "/*.zip; do s=$(unzip -p \"$z\" "
            "metadata.json | sed -n 's/.*\"schema\": \"\\([a-z0-9]*\\)\".*/\\1/p'); "
            "unzip -p \"$z\" metadata.json > ", root, "/metadata.$s.json; done");
      }
      const std::string csvPath =
          root + "/xnas-itch-20220610." + spec.dbnSchema + ".csv";
      st.rep.csvPath = csvPath;
      st.rep.jobId   = md.jobId;

      ARROW_ASSIGN_OR_RAISE(const auto tc,
                            csv::transcodeCsv(csvPath, md, opts.dbnVersion));
      st.rep.csvRecords         = tc.records;
      st.rep.pullLimit          = md.limit;
      st.rep.truncatedByLimit   = tc.truncatedByLimit;
      st.rep.recordsAtLastTsEvent = tc.recordsAtLastTsEvent;
      st.rep.tsRecvMatchesDelta = tc.tsRecvMatchesDelta;
      st.rep.actionCounts       = tc.actionCounts;
      st.rep.requestedFromNs    = static_cast<std::int64_t>(md.start);
      st.rep.requestedToNs      = static_cast<std::int64_t>(md.end);
      st.rep.recordedFromNs     = static_cast<std::int64_t>(md.start);
      st.rep.recordedToNs       = static_cast<std::int64_t>(tc.deliveredEnd(md));
      st.mappings               = tc.mappings;
      st.dataset                = md.dataset;

      if (opts.forceReencode && fileExists(dbnPath)) std::remove(dbnPath.c_str());
      ARROW_ASSIGN_OR_RAISE(const auto ret, csv::retain(dbnPath, tc.dbn));
      st.rep.dbnExisted   = ret.existed;
      st.rep.dbnRewritten = ret.changed;
      st.rep.dbnBytes     = ret.bytes;

      // Read the file BACK FROM DISK rather than reusing `tc.dbn`. The insert
      // path is the reload path; if retention wrote something other than what
      // it was handed, this is where it shows.
      ARROW_ASSIGN_OR_RAISE(dbnBytes, csv::readFile(dbnPath));
      if (dbnBytes != tc.dbn) {
        return arrow::Status::IOError(
            "the retained DBN at ", dbnPath,
            " does not read back as the bytes just written. L1 is a rebuildable "
            "projection of these files (SPEC D6); a file that does not read "
            "back is not a baseline.");
      }
    }
    st.rep.dbnBytes = dbnBytes.size();

    const std::uint32_t sessionId =
        opts.sessionId != 0
            ? opts.sessionId
            : deriveSessionId(opts.fromDbnOnly ? "XNAS.ITCH" : md.dataset,
                              spec.dbnSchema,
                              opts.fromDbnOnly ? std::string("from-dbn") : md.jobId);

    // ---- decode, twice, at two chunkings -------------------------------
    ARROW_ASSIGN_OR_RAISE(const auto whole,
                          decodeDbn(dbnBytes, sessionId, dbnBytes.size()));
    // 8191: a prime-ish chunk that lands inside the 100-byte metadata block,
    // inside the variable symbology section and inside records of both 56 and
    // 48 bytes, so no boundary in the file is also a feed boundary.
    ARROW_ASSIGN_OR_RAISE(const auto chunked, decodeDbn(dbnBytes, sessionId, 8191));

    std::ostringstream note;
    bool ok = true;
    if (whole.ticks.size() != chunked.ticks.size() ||
        whole.book.size() != chunked.book.size()) {
      ok = false;
      note << "record counts differ between a whole-file feed and 8191-byte "
              "feeds: ticks "
           << whole.ticks.size() << " vs " << chunked.ticks.size() << ", book "
           << whole.book.size() << " vs " << chunked.book.size() << ". ";
    } else {
      // The PODs are trivially copyable and have no padding holes that are not
      // asserted (RecordHeader is static_assert'd at 40 bytes), so memcmp is
      // the whole comparison rather than a field list that could omit one.
      if (!whole.ticks.empty() &&
          std::memcmp(whole.ticks.data(), chunked.ticks.data(),
                      whole.ticks.size() * sizeof(TickRecord)) != 0) {
        ok = false;
        note << "TickRecords differ byte-for-byte between the two chunkings. ";
      }
      if (!whole.book.empty() &&
          std::memcmp(whole.book.data(), chunked.book.data(),
                      whole.book.size() * sizeof(BookRecord)) != 0) {
        ok = false;
        note << "BookRecords differ byte-for-byte between the two chunkings. ";
      }
    }
    if (ok) {
      note << "whole-file and 8191-byte-chunk feeds of the retained DBN produce "
              "byte-identical canonical records ("
           << whole.ticks.size() << " ticks, " << whole.book.size() << " book)";
      if (st.rep.dbnExisted && !st.rep.dbnRewritten && !opts.fromDbnOnly) {
        note << "; re-transcoding the CSV reproduced the retained file "
                "byte-for-byte";
      }
    }
    st.rep.roundTripOk   = ok;
    st.rep.roundTripNote = note.str();
    if (!ok) {
      return arrow::Status::Invalid("round-trip failed for ", spec.kind, ": ",
                                    st.rep.roundTripNote);
    }

    st.ticks = whole.ticks;
    st.book  = whole.book;
    st.rep.decodedRecords  = whole.stats.recordsIn;
    st.rep.canonicalRows   = whole.ticks.size() + whole.book.size();
    st.rep.sideUnspecified = whole.stats.sideUnspecified;
    st.rep.symbol          = whole.symbol;
    st.rep.venue           = whole.venue;
    st.rep.instrumentId    = whole.instrumentId;
    if (st.dataset.empty()) st.dataset = whole.meta.dataset;

    if (opts.fromDbnOnly) {
      // The window from the DBN's own metadata — the whole point of writing
      // `limit` into it.
      st.rep.pullLimit        = whole.meta.limit;
      st.rep.requestedFromNs  = static_cast<std::int64_t>(whole.meta.start);
      st.rep.requestedToNs    = static_cast<std::int64_t>(whole.meta.end);
      st.rep.truncatedByLimit =
          whole.meta.limit != 0 && whole.stats.recordsIn == whole.meta.limit;
      std::int64_t lastTs = 0;
      for (const auto& t : whole.ticks) lastTs = std::max(lastTs, t.header.tsEventNs);
      for (const auto& b : whole.book) lastTs = std::max(lastTs, b.header.tsEventNs);
      st.rep.recordedFromNs = st.rep.requestedFromNs;
      st.rep.recordedToNs =
          st.rep.truncatedByLimit ? lastTs : st.rep.requestedToNs;
      st.rep.mappingsFromDbn = true;
      for (const auto& m : whole.meta.mappings) {
        for (const auto& iv : m.intervals) {
          std::uint32_t id = 0;
          try {
            id = static_cast<std::uint32_t>(std::stoul(iv.symbol));
          } catch (...) {
            continue;
          }
          st.mappings.push_back(csv::Transcoded::Mapping{
              id, m.rawSymbol, iv.startDate,
              iv.endDate > 0 ? iv.endDate - 1 : iv.startDate});
        }
      }
    }

    if (st.rep.recordedToNs <= st.rep.recordedFromNs) {
      return arrow::Status::Invalid(
          "the delivered window for ", spec.kind, " came out empty (",
          formatWindow(st.rep.recordedFromNs, st.rep.recordedToNs),
          "). Writing it as an ingested window would assert coverage of "
          "nothing.");
    }

    staged.push_back(std::move(st));
  }

  rep.sessionId = opts.sessionId != 0 ? opts.sessionId : 0;  // per-kind; see below

  if (opts.noInsert) {
    for (auto& st : staged) rep.kinds.push_back(st.rep);
    return rep;
  }

  // ---- pass 2: drop, insert, record -------------------------------------
  for (auto& st : staged) {
    const std::uint32_t sessionId =
        opts.sessionId != 0
            ? opts.sessionId
            : deriveSessionId(st.dataset, st.rep.dbnSchema,
                              st.rep.jobId.empty() ? std::string("from-dbn")
                                                   : st.rep.jobId);
    if (rep.sessionId == 0) rep.sessionId = sessionId;

    // Day partitions this kind's rows land in.
    std::set<std::uint32_t> days;
    for (const auto& t : st.ticks) days.insert(dayPartitionOf(t.header.tsEventNs));
    for (const auto& b : st.book) days.insert(dayPartitionOf(b.header.tsEventNs));
    // The recorded window's own days too: a window may claim a day the rows do
    // not reach (a quiet tail), and that day's partition is still being
    // asserted over.
    for (std::uint32_t d = dayPartitionOf(st.rep.recordedFromNs);;) {
      days.insert(d);
      const auto nextMidnight = csv::midnightUtcNs(d) + 86400000000000ULL;
      if (static_cast<std::int64_t>(nextMidnight) >= st.rep.recordedToNs) break;
      d = csv::yyyymmddUtc(nextMidnight);
    }

    // ---- THE INVARIANT THE SCHEMA CANNOT ENFORCE -----------------------
    //
    // From the ⚠ block in clickhouse.sql: a DROP PARTITION invalidates EVERY
    // symbol's completeness rows for that partition, not just the symbol being
    // re-ingested, because partitions are day-granular while completeness is
    // per symbol. Without this step another symbol's newest row still reads
    // state=1 over an emptied partition.
    //
    // So: before dropping, find every OTHER symbol with a recorded window for
    // this kind intersecting a partition about to go, and write it a state=3
    // (invalidated) row. Our own symbol gets state=0 below, which covers it.
    for (const std::uint32_t day : days) {
      const std::int64_t dayFrom = static_cast<std::int64_t>(csv::midnightUtcNs(day));
      const std::int64_t dayTo   = dayFrom + 86400000000000LL;
      std::ostringstream q;
      q << "SELECT DISTINCT symbol, toUnixTimestamp64Nano(from_ts), "
           "toUnixTimestamp64Nano(to_ts) FROM gma.ingest_windows WHERE kind = '"
        << st.rep.kind << "' AND symbol != '" << st.rep.symbol
        << "' AND from_ts < toDateTime64(" << dayTo
        << "/1000000000, 9, 'UTC') AND to_ts > toDateTime64(" << dayFrom
        << "/1000000000, 9, 'UTC') FORMAT TabSeparated";
      ARROW_ASSIGN_OR_RAISE(const auto others, client.select(q.str()));
      for (const auto& r : others) {
        if (r.size() != 3) continue;
        ARROW_RETURN_NOT_OK(writeWindowRow(
            client, r[0], st.rep.kind, std::stoll(r[1]), std::stoll(r[2]),
            /*state=*/3, /*rowsInserted=*/0, sessionId,
            "invalidated by DROP PARTITION " + std::to_string(day) +
                " for " + st.rep.symbol));
        rep.invalidationRows.push_back(r[0] + "/" + st.rep.kind + " " + r[1] +
                                       ".." + r[2] + " -> state=3 (partition " +
                                       std::to_string(day) + ")");
      }
    }

    // ---- state=0, in flight --------------------------------------------
    ARROW_RETURN_NOT_OK(writeWindowRow(client, st.rep.symbol, st.rep.kind,
                                       st.rep.recordedFromNs, st.rep.recordedToNs,
                                       /*state=*/0, /*rowsInserted=*/0, sessionId,
                                       st.rep.jobId));

    // ---- DROP PARTITION, then reload (D5) ------------------------------
    for (const std::uint32_t day : days) {
      std::ostringstream q;
      q << "ALTER TABLE gma." << st.table << " DROP PARTITION " << day;
      ARROW_RETURN_NOT_OK(client.exec(q.str()).status());
      rep.partitionsDropped.push_back(st.table + ":" + std::to_string(day));
    }

    // ---- sort, then insert, per day partition --------------------------
    std::ostringstream body;
    std::size_t inserted = 0;
    std::vector<SortKey> keys;

    if (st.rep.kind == "trades") {
      std::vector<const TickRecord*> ptrs;
      ptrs.reserve(st.ticks.size());
      for (const auto& t : st.ticks) ptrs.push_back(&t);
      std::stable_sort(ptrs.begin(), ptrs.end(),
                       [&](const TickRecord* a, const TickRecord* b) {
                         return SortKey{st.rep.symbol, a->header.tsEventNs,
                                        a->header.providerSeq} <
                                SortKey{st.rep.symbol, b->header.tsEventNs,
                                        b->header.providerSeq};
                       });
      body << "INSERT INTO gma.trades (" << kTradesColumns
           << ") FORMAT TabSeparated\n";
      for (const auto* t : ptrs) {
        appendTradeRow(body, *t, st.rep.symbol, st.rep.venue);
        keys.push_back(SortKey{st.rep.symbol, t->header.tsEventNs,
                               t->header.providerSeq});
        ++inserted;
      }
    } else {
      std::vector<const BookRecord*> ptrs;
      ptrs.reserve(st.book.size());
      for (const auto& b : st.book) ptrs.push_back(&b);
      std::stable_sort(ptrs.begin(), ptrs.end(),
                       [&](const BookRecord* a, const BookRecord* b) {
                         return SortKey{st.rep.symbol, a->header.tsEventNs,
                                        a->header.providerSeq} <
                                SortKey{st.rep.symbol, b->header.tsEventNs,
                                        b->header.providerSeq};
                       });
      body << "INSERT INTO gma.book_events (" << kBookEventsColumns
           << ") FORMAT TabSeparated\n";
      for (const auto* b : ptrs) {
        appendBookRow(body, *b, st.rep.symbol, st.rep.venue);
        keys.push_back(SortKey{st.rep.symbol, b->header.tsEventNs,
                               b->header.providerSeq});
        ++inserted;
      }
    }

    // ENC-1484's number, free while the keys are in hand: D5 picked plain
    // MergeTree because this sort key is NOT unique, and here is by how much.
    std::vector<SortKey> uniq = keys;
    std::sort(uniq.begin(), uniq.end());
    const auto last = std::unique(uniq.begin(), uniq.end());
    st.rep.distinctSortKeys = static_cast<std::size_t>(last - uniq.begin());
    st.rep.duplicateSortKeys = keys.size() - st.rep.distinctSortKeys;

    const arrow::Status ins =
        inserted == 0 ? arrow::Status::OK() : client.exec(body.str()).status();
    if (!ins.ok()) {
      // state=2, failed. Written before returning so the window never reads as
      // anything but unloaded: the state=0 row above is already the newest, and
      // this makes the failure explicit rather than leaving it in flight
      // forever.
      ARROW_RETURN_NOT_OK(writeWindowRow(client, st.rep.symbol, st.rep.kind,
                                         st.rep.recordedFromNs,
                                         st.rep.recordedToNs, /*state=*/2, 0,
                                         sessionId, st.rep.jobId));
      return ins;
    }
    st.rep.rowsInserted = inserted;

    // ---- symbology (SPEC D7, docs §8.9) --------------------------------
    for (const auto& m : st.mappings) {
      const std::uint64_t validFrom = csv::midnightUtcNs(m.firstDay);
      // HALF-OPEN: [valid_from, valid_to). The DBN interval is already
      // half-open in days, so valid_to is midnight of the day AFTER the last
      // day the mapping was observed on.
      const std::uint64_t validTo = csv::midnightUtcNs(m.lastDay) + 86400000000000ULL;
      std::ostringstream q;
      q << "INSERT INTO gma.symbology (dataset, instrument_id, raw_symbol, "
           "valid_from, valid_to, source_ref, ingested_at) SELECT '"
        << st.dataset << "', " << m.instrumentId << ", '" << m.rawSymbol
        << "', toDateTime64(" << validFrom << "/1000000000, 9, 'UTC'), "
           "toDateTime64("
        << validTo << "/1000000000, 9, 'UTC'), '"
        << (st.rep.jobId.empty() ? st.rep.dbnPath : st.rep.jobId)
        << "', now64(3)";
      ARROW_RETURN_NOT_OK(client.exec(q.str()).status());
      ++rep.symbologyRows;
    }

    // ---- state=1, complete ---------------------------------------------
    ARROW_RETURN_NOT_OK(writeWindowRow(client, st.rep.symbol, st.rep.kind,
                                       st.rep.recordedFromNs, st.rep.recordedToNs,
                                       /*state=*/1, inserted, sessionId,
                                       st.rep.jobId));
    rep.kinds.push_back(st.rep);
  }

  return rep;
}

}  // namespace gma::data::ingest
