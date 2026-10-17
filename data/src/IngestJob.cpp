//
// IngestJob — see IngestJob.hpp for the path and what is deliberately not here.
//

#include "gma/data/IngestJob.hpp"

#include "gma/data/DatabentoAdapter.hpp"
#include "gma/data/SymbolTable.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <span>
#include <sstream>
#include <vector>

namespace gma::data::ingest {

// ===========================================================================
// THE TWO RULINGS THIS TICKET OWED. Text, so that the tool's output, the Linear
// comment and SPEC §5 cannot drift into three different answers.
// ===========================================================================

const char* const kSessionIdRuling =
    "A RE-INGEST REUSES ITS session_id, AND session_id IS THE IDENTITY OF THE "
    "RETAINED DBN FILE RATHER THAN A PER-RUN COUNTER.\n"
    "  session_id = FNV-1a-32 over the whole retained .dbn, forced non-zero. "
    "The same bytes give the same number on every machine, every rebuild and "
    "every reload; a CORRECTED pull is different bytes and therefore a "
    "different session, which is the one case where the distinction is real.\n"
    "  Decided this way because the alternatives break locked decisions. D6 "
    "makes L1 'a derived, rebuildable projection of the raw DBN files'; a "
    "projection whose output changes when rebuilt from unchanged input has a "
    "hidden input, and a per-run counter is exactly that. D5's re-ingest ('DROP "
    "PARTITION + reload reproduces identical rows') and ENC-1487's "
    "bit-identical assertions both fail on the session_id column alone if a "
    "reload renumbers it. And DatabentoAdapter's own note on provider_seq -- "
    "'FROM DATABENTO, NEVER SYNTHESIZED ... stable across a re-download, which "
    "is what makes re-ingest converge and the splice's dedup exact' -- is about "
    "the PAIR (session_id, provider_seq) (docs 8.3); a fresh session_id each run "
    "makes the stable half useless.\n"
    "  WHY THE FILE AND NOT THE VENDOR job_id, WHICH WAS THE FIRST ANSWER: the "
    "job_id is in metadata.json and is NOT IN DBN AT ALL. Deriving from it "
    "makes `gma_ingest reload` -- the pure D6 reload, which reads only the "
    "retained file -- produce a DIFFERENT session_id from the run that created "
    "it, so the reload's rows would differ from the originals in exactly the "
    "column that is supposed to prove they do not. Measured on this sample "
    "before the change. The job_id stays on every ingest_windows and symbology "
    "row as `source_ref`, so nothing traceable is lost; it is provenance, not "
    "identity.\n"
    "  AND A CORRECTION TO docs/data-layer.md 8.8 THAT NO ANSWER HERE RESCUES: "
    "8.8 calls (symbol, kind, from_ts, session_id) 'unique by construction'. "
    "THAT IS FALSE UNDER EVERY ANSWER, not only under reuse. ENC-1480 made the "
    "table an append-only transition log, so one window and one session write at "
    "least two rows (state=0 then state=1) and a failed run writes three -- "
    "observed on this ingest as exactly 4 rows for 2 windows. The claim dates "
    "from revision 2's one-row-per-window ReplacingMergeTree and was voided by "
    "the rewrite that replaced it. What makes the table readable is the READ "
    "RULE, never uniqueness.";

const char* const kGapsNote =
    "ZERO gma.gaps ROWS WRITTEN, AND NOT FOR LACK OF SCOPE: THE SEQ-"
    "DISCONTINUITY DETECTOR CANNOT RUN ON A SYMBOL-FILTERED PULL.\n"
    "  docs 8.8 detects a provider hole as 'a seq discontinuity'. Databento's "
    "`sequence` is the VENUE's, not the symbol's: in this sample consecutive "
    "MSFT records step 11404021 -> 11404028 -> 11423666, because every other "
    "Nasdaq instrument's messages occupy the numbers in between. provider_seq is "
    "MONOTONIC within (session_id, symbol) exactly as 8.3 says -- measured "
    "sorted, 1000/1000, both kinds -- but it is not CONTIGUOUS, and gap "
    "detection needs contiguity. A gap row per jump would have produced hundreds "
    "of phantom holes per kind, each of which a research run joining `gaps` "
    "reads as 'there was no data here': the same wrong-answer class the table "
    "exists to prevent, manufactured by the detector. A real detector for a "
    "filtered pull needs either an unfiltered channel feed or the vendor's own "
    "condition.json (which for 2022-06-10 reads 'available'), and that is a "
    "ticket, not a line in this one.";

namespace {

// ---------------------------------------------------------------------------
// session_id derivation — see kSessionIdRuling.
// ---------------------------------------------------------------------------
std::uint32_t deriveSessionId(const std::vector<std::byte>& dbnBytes) {
  // FNV-1a 32 over the retained file. Chosen for being a fixed, written-down
  // function of its input rather than for its distribution: the property that
  // matters is that the same bytes yield the same number on every machine,
  // every rebuild and every reload. The file already contains the dataset, the
  // schema, the requested window, the limit and the symbology in its metadata
  // block, so hashing the file subsumes hashing those.
  std::uint32_t h = 0x811C9DC5u;
  for (const std::byte b : dbnBytes) {
    h ^= static_cast<std::uint8_t>(b);
    h *= 0x01000193u;
  }
  // 0 is reserved: a zero session_id on an event row is indistinguishable from
  // a default-constructed header.
  return h == 0 ? 1u : h;
}

// CODE-REVIEW FINDING #8. Every other line in this file is scrupulous about
// returning a `Status`; four used `std::stoll`/`std::stoul`, which THROW. The
// inputs are ClickHouse response fields, where `\N`, an empty field or a
// truncated body are all reachable — and an exception escaping an
// `arrow::Status`-returning function skips every refusal path this job has,
// including the `state=2` write that records a failed window.
template <typename T>
bool parseExact(const std::string& text, T& out) {
  if (text.empty()) return false;
  const char* first = text.data();
  const char* last  = text.data() + text.size();
  const auto r = std::from_chars(first, last, out);
  return r.ec == std::errc{} && r.ptr == last;
}

arrow::Status badField(const char* what, const std::string& text) {
  return arrow::Status::Invalid(
      "ClickHouse returned \"", text, "\" for ", what,
      ", which is not an integer. This is a COULD-NOT-RUN rather than a "
      "finding: no verdict about the data was reached.");
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
// The chunk size is a parameter because it is one of the round-trip's two axes:
// the adapter buffers across `feed()` calls, so a chunk that straddles the
// metadata block, the symbology section and records of both 56 and 48 bytes
// drives the framing differently from one whole-file feed. Two chunkings
// agreeing is a real property; one chunking repeated is not.
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
  // metadata's symbology (SPEC D7) rather than asserted by us, which is why
  // the symbology section is written into the DBN at all.
  //
  // `fillReducesRestingOrder` IS LEFT AT ITS DEFAULT, AND THE DEFAULT IS NOW
  // MEASURED rather than assumed — see the comment on it in
  // DatabentoAdapter.hpp. 91 of 91 `F` records in this sample are accompanied
  // by a `C` for the SAME order_id at the same (ts_event, sequence), so the
  // venue emits the resting reduction itself and applying the fill as a second
  // reduction would double-reduce on 100% of fills.
  DatabentoAdapter adapter(cfg, symbols, venues);

  if (chunk == 0) return arrow::Status::Invalid("chunk size 0");
  for (std::size_t off = 0; off < bytes.size(); off += chunk) {
    const std::size_t n = std::min(chunk, bytes.size() - off);
    ARROW_RETURN_NOT_OK(
        adapter.feed(std::span<const std::byte>(bytes.data() + off, n)));
  }
  ARROW_RETURN_NOT_OK(adapter.finish());

  Decoded d;
  d.ticks = adapter.takeTrades();
  d.book  = adapter.takeBookEvents();
  d.stats = adapter.stats();
  if (!adapter.metadata().has_value()) {
    return arrow::Status::Invalid("the adapter produced no DBN metadata");
  }
  d.meta = *adapter.metadata();

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
    return asFinding(arrow::Status::Invalid(
        "decode yielded ", symbolIds.size(), " symbol(s) and ",
        instrumentIds.size(),
        " instrument_id(s). This slice is one symbol (SPEC 6) and the window is "
        "recorded PER SYMBOL in gma.ingest_windows, so a multi-symbol file is "
        "refused rather than recorded under one of them."));
  }
  d.symbol       = symbols.name(*symbolIds.begin());
  d.venue        = venues.name(venueId);
  d.instrumentId = *instrumentIds.begin();
  return d;
}

// ---------------------------------------------------------------------------
// Row rendering. ONE renderer, used twice: for the round-trip comparison and
// for the insert body. That is deliberate — comparing the RENDERED rows is
// what makes the round-trip a statement about what reaches L1, and a second
// renderer written for the comparison could agree with itself while disagreeing
// with the insert.
//
// Columns are NAMED in the INSERT so a DDL column reorder is a server-side
// error rather than a silently transposed table.
// ---------------------------------------------------------------------------
constexpr const char* kTradesColumns =
    "ts_event, ts_recv, provider_seq, session_id, symbol, instrument_id, venue, "
    "price, size, aggressor, book_neutral, trade_id, flags";

constexpr const char* kBookEventsColumns =
    "ts_event, ts_recv, provider_seq, session_id, symbol, instrument_id, venue, "
    "action, side, price, size, order_id, priority, depth, epoch, flags";

// A field this renderer omits is a field no round-trip assertion covers and no
// L1 column holds, so the two facts stay tied together. TickRecord's quote half
// (bidPx/askPx/bidSz/askSz/bidCt/askCt) is omitted because `gma.quotes` is not
// in this slice (SPEC Non-goals) and nothing populates it.
std::string renderTradeRow(const TickRecord& t, const std::string& symbol,
                           const std::string& venue) {
  std::ostringstream os;
  os << t.header.tsEventNs << '\t' << t.header.tsRecvNs << '\t'
     << t.header.providerSeq << '\t' << t.header.sessionId << '\t'
     << ch::tsvEscape(symbol) << '\t' << t.header.instrumentId << '\t'
     << ch::tsvEscape(venue) << '\t' << t.price << '\t' << t.size << '\t'
     << static_cast<unsigned>(t.aggressor) << '\t' << (t.bookNeutral ? 1 : 0)
     << '\t' << t.tradeId << '\t' << t.flags << '\n';
  return os.str();
}

std::string renderBookRow(const BookRecord& b, const std::string& symbol,
                          const std::string& venue) {
  std::ostringstream os;
  os << b.header.tsEventNs << '\t' << b.header.tsRecvNs << '\t'
     << b.header.providerSeq << '\t' << b.header.sessionId << '\t'
     << ch::tsvEscape(symbol) << '\t' << b.header.instrumentId << '\t'
     << ch::tsvEscape(venue) << '\t' << static_cast<unsigned>(b.action) << '\t'
     << static_cast<unsigned>(b.side) << '\t' << b.price << '\t' << b.size << '\t'
     << b.orderId << '\t' << b.priority << '\t' << b.depth << '\t' << b.epoch
     << '\t' << b.flags << '\n';
  return os.str();
}

std::string renderRows(const Decoded& d) {
  std::string out;
  for (const auto& t : d.ticks) out += renderTradeRow(t, d.symbol, d.venue);
  for (const auto& b : d.book) out += renderBookRow(b, d.symbol, d.venue);
  return out;
}

// ---------------------------------------------------------------------------
// ingest_windows writes. updated_at IS STAMPED ON EVERY ROW, including the
// state=0 one. The DDL's own measured note: with no value the column defaults
// to epoch 0, every row ties, "the single newest row" becomes arbitrary and a
// stale state=1 can win — the identical zero-valued-clock defect the table was
// rewritten to eliminate. `DEFAULT now64(3)` in the DDL is the backstop;
// writing it is the contract.
//
// `now64(3)` is a FUNCTION, and TabSeparated would store the literal text, so
// these rows go in through a SELECT. The clock is the server's either way.
// ---------------------------------------------------------------------------
arrow::Status writeWindowRow(const ch::Client& client, const std::string& symbol,
                             const std::string& kind, std::int64_t fromNs,
                             std::int64_t toNs, std::uint8_t state,
                             std::uint64_t rowsInserted, std::uint32_t sessionId,
                             const std::string& sourceRef) {
  std::ostringstream q;
  q << "INSERT INTO gma.ingest_windows (symbol, kind, from_ts, to_ts, state, "
       "rows_inserted, updated_at, completed_at, session_id, source, source_ref) "
       "SELECT "
    << ch::sqlQuote(symbol) << ", " << ch::sqlQuote(kind)
    << ", fromUnixTimestamp64Nano(toInt64(" << fromNs
    << "), 'UTC'), fromUnixTimestamp64Nano(toInt64(" << toNs << "), 'UTC'), "
    << static_cast<unsigned>(state) << ", " << rowsInserted << ", now64(3), "
    // completed_at: a COMPLETION clock. Zero unless this row IS the completion
    // — the DDL is explicit that it is zero while in flight and that ordering
    // by it is what made the revision-2 engine delete the in-flight row.
    << (state == 1 ? "now64(3)" : "fromUnixTimestamp64Milli(toInt64(0), 'UTC')")
    << ", " << sessionId << ", 'databento', " << ch::sqlQuote(sourceRef);
  return client.exec(q.str()).status();
}

std::string isoFromNs(std::int64_t ns) {
  const auto day = csv::yyyymmddUtc(static_cast<std::uint64_t>(ns));
  const auto midnight = static_cast<std::int64_t>(csv::midnightUtcNs(day));
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

// The partition days a window and its rows touch.
std::set<std::uint32_t> daysOf(std::int64_t fromNs, std::int64_t toNs) {
  std::set<std::uint32_t> days;
  std::uint32_t d = csv::yyyymmddUtc(static_cast<std::uint64_t>(fromNs));
  for (;;) {
    days.insert(d);
    const auto next = csv::midnightUtcNs(d) + 86400000000000ULL;
    if (static_cast<std::int64_t>(next) >= toNs) break;
    d = csv::yyyymmddUtc(next);
  }
  return days;
}

struct Staged {
  KindReport rep;
  Decoded decoded;
  std::vector<csv::Transcoded::Mapping> mappings;
  std::string table;
  std::string dataset;
  std::uint32_t sessionId = 0;
};

}  // namespace

// ===========================================================================
// retainOnlyIfDecodable — see IngestJob.hpp for what the old order destroyed.
// ===========================================================================

arrow::Result<csv::RetentionResult> retainOnlyIfDecodable(
    const std::string& path, const std::vector<std::byte>& bytes) {
  // The decode is done for its STATUS, not its records: the question is only
  // whether the adapter accepts these bytes end to end, including the metadata
  // block, the symbology section and `finish()`'s refusal of a stream that ends
  // mid-record. `sessionId` is irrelevant to decodability, so a fixed 1 is
  // passed rather than threading the real one in and implying it matters.
  const arrow::Status decodes = decodeDbn(bytes, 1u, bytes.size()).status();
  if (!decodes.ok()) {
    return asFinding(arrow::Status::Invalid(
        "REFUSING to write ", path,
        ": the bytes do not decode through DatabentoAdapter, and that file is "
        "the SPEC D6 baseline L1 is rebuilt from — overwriting it with "
        "undecodable bytes is what makes a later `reload` fail permanently. "
        "Nothing was written; any existing file is untouched.\n  the decoder "
        "said: ",
        decodes.message()));
  }
  return csv::retain(path, bytes);
}

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
  // Projection plus a WHERE on the sort-key prefix. No aggregate, no FINAL, no
  // argMax, no LIMIT: the version resolution and the coverage test are both
  // done by the caller, which is the DDL's own instruction and keeps this a
  // plain selection on this side of the D2 boundary.
  std::ostringstream q;
  q << "SELECT toUnixTimestamp64Nano(from_ts), toUnixTimestamp64Nano(to_ts), "
       "toUInt16(state), toUnixTimestamp64Milli(updated_at) "
       "FROM gma.ingest_windows WHERE symbol = "
    << ch::sqlQuote(symbol) << " AND kind = " << ch::sqlQuote(kind)
    << " FORMAT TabSeparated";
  ARROW_ASSIGN_OR_RAISE(const auto rows, client.select(q.str()));

  std::vector<IngestWindowRow> parsed;
  parsed.reserve(rows.size());
  for (const auto& r : rows) {
    if (r.size() != 4) {
      return arrow::Status::Invalid("gma.ingest_windows row has ", r.size(),
                                    " field(s), expected 4");
    }
    IngestWindowRow w;
    std::uint16_t state = 0;
    if (!parseExact(r[0], w.fromTsNs)) return badField("from_ts", r[0]);
    if (!parseExact(r[1], w.toTsNs)) return badField("to_ts", r[1]);
    if (!parseExact(r[2], state)) return badField("state", r[2]);
    if (!parseExact(r[3], w.updatedAtMs)) return badField("updated_at", r[3]);
    w.state = static_cast<std::uint8_t>(state);
    parsed.push_back(w);
  }
  return isComplete(parsed, reqFromNs, reqToNs);
}

arrow::Result<std::int64_t> rowCount(const ch::Client& client,
                                     const std::string& table,
                                     const std::string& symbol,
                                     std::int64_t fromNs, std::int64_t toNs) {
  std::ostringstream q;
  // `table` is NOT interpolated from user input — it is one of two string
  // literals chosen by the caller's `KindSpec` — but `symbol` is, and comes
  // from vendor DBN symbology as well as from the CLI.
  q << "SELECT count() FROM gma." << table << " WHERE symbol = "
    << ch::sqlQuote(symbol) << " AND ts_event >= fromUnixTimestamp64Nano(toInt64("
    << fromNs << "), 'UTC') AND ts_event < fromUnixTimestamp64Nano(toInt64("
    << toNs << "), 'UTC')";
  ARROW_ASSIGN_OR_RAISE(const auto text, client.scalar(q.str()));
  std::int64_t n = 0;
  if (!parseExact(text, n)) return badField("count()", text);
  return n;
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
  // schema `mbo` and L1's table is `book_events` — and conflating them writes a
  // completeness row under a kind no reader queries.
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
  // Every file becomes canonical rows BEFORE anything is dropped or inserted. A
  // DROP PARTITION followed by a decode failure would leave the partition empty
  // with nothing but a state=0 able to say so; doing the whole raw half first
  // means a bad file is refused with L1 untouched.
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
    std::vector<csv::Transcoded::Probe> probes;
    bool retainedMatched = false;

    if (opts.fromDbnOnly) {
      // THE PURE D6 RELOAD. No CSV and no metadata.json: the retained DBN
      // carries `start`, `end` and `limit` in its own metadata block, which is
      // what makes "reload must reload from something" a complete statement.
      if (!fileExists(dbnPath)) {
        return arrow::Status::IOError(
            "--from-dbn-only but ", dbnPath,
            " does not exist. SPEC D6 retains the raw DBN so a reload needs no "
            "re-pull; run once without the flag to produce it from the CSV.");
      }
      ARROW_ASSIGN_OR_RAISE(dbnBytes, csv::readFile(dbnPath));
      st.rep.dbnExisted = true;
      st.dataset        = "XNAS.ITCH";  // replaced from the DBN metadata below
    } else {
      // metadata.json: PER JOB, and this directory holds two jobs.
      //
      // Each vendor zip carries its own metadata.json, and extracting both zips
      // into one directory leaves ONE of them — silently the wrong one for one
      // of the two CSVs. So a per-schema sidecar is preferred, and a shared
      // metadata.json is accepted ONLY if its own `query.schema` matches the
      // file being read. A mismatch is refused with the command that fixes it,
      // because the fields at stake (`start`, `end`, `limit`) are exactly what
      // gma.ingest_windows forms an opinion from.
      const std::string sidecar = root + "/metadata." + spec.dbnSchema + ".json";
      const std::string shared  = root + "/metadata.json";
      const std::string mdPath  = fileExists(sidecar) ? sidecar : shared;
      ARROW_ASSIGN_OR_RAISE(const auto md, csv::readPullMetadata(mdPath));
      if (md.schema != spec.dbnSchema) {
        return arrow::Status::Invalid(
            mdPath, " describes the `", md.schema, "` job (job_id ", md.jobId,
            ") but the `", spec.dbnSchema,
            "` CSV is being read. Two batch jobs extracted into one directory "
            "leave one metadata.json, and `start`/`end`/`limit` from the wrong "
            "job would be recorded in gma.ingest_windows as this pull's window. "
            "Extract the per-job sidecars first, e.g.\n"
            "    for z in ", root,
            "/*.zip; do s=$(unzip -p \"$z\" metadata.json | "
            "python3 -c 'import json,sys; print(json.load(sys.stdin)[\"query\"]"
            "[\"schema\"])'); unzip -p \"$z\" metadata.json > ", root,
            "/metadata.$s.json; done");
      }
      const std::string csvPath =
          root + "/xnas-itch-20220610." + spec.dbnSchema + ".csv";
      st.rep.csvPath = csvPath;
      st.rep.jobId   = md.jobId;
      st.dataset     = md.dataset;

      ARROW_ASSIGN_OR_RAISE(const auto tc,
                            csv::transcodeCsv(csvPath, md, opts.dbnVersion));
      st.rep.csvRecords           = tc.records;
      st.rep.pullLimit            = md.limit;
      st.rep.truncatedByLimit     = tc.truncatedByLimit;
      st.rep.recordsAtLastTsEvent = tc.recordsAtLastTsEvent;
      st.rep.tsRecvMatchesDelta   = tc.tsRecvMatchesDelta;
      st.rep.actionCounts         = tc.actionCounts;
      st.rep.requestedFromNs      = static_cast<std::int64_t>(md.start);
      st.rep.requestedToNs        = static_cast<std::int64_t>(md.end);
      st.rep.recordedFromNs       = static_cast<std::int64_t>(md.start);
      st.rep.recordedToNs         = static_cast<std::int64_t>(tc.deliveredEnd(md));
      st.mappings                 = tc.mappings;
      probes                      = tc.probes;

      // DECODE, THEN RETAIN (finding #3). `retainOnlyIfDecodable` proves the
      // bytes decode through the real adapter BEFORE the D6 baseline moves, so
      // a bad `--dbn-version`, a bad CSV or an encoder regression fails with
      // the previous file intact instead of destroying it.
      //
      // `--force-reencode` deletes the existing file, so the deletion also
      // waits until the replacement has been proven: the old order deleted
      // first and could leave NO file at all.
      {
        const arrow::Status decodes =
            decodeDbn(tc.dbn, 1u, tc.dbn.size()).status();
        if (!decodes.ok()) {
          return asFinding(arrow::Status::Invalid(
              "the DBN transcoded from ", csvPath,
              " does not decode through DatabentoAdapter, so it was NOT "
              "written: ",
              decodes.message()));
        }
        if (opts.forceReencode && fileExists(dbnPath)) {
          std::remove(dbnPath.c_str());
        }
      }
      ARROW_ASSIGN_OR_RAISE(const auto ret,
                            retainOnlyIfDecodable(dbnPath, tc.dbn));
      st.rep.dbnExisted   = ret.existed;
      st.rep.dbnRewritten = ret.changed;
      retainedMatched     = ret.existed && !ret.changed;

      // Read the file BACK FROM DISK rather than reusing `tc.dbn`. The insert
      // path IS the reload path; if retention wrote something other than what
      // it was handed, this is where it shows.
      ARROW_ASSIGN_OR_RAISE(dbnBytes, csv::readFile(dbnPath));
      if (dbnBytes != tc.dbn) {
        // A FINDING (exit 1), not a could-not-run: the file was written and
        // read back fine, and what came back is wrong. Was IOError, which
        // `main` mapped to exit 2 — finding #1.
        return asFinding(arrow::Status::Invalid(
            "the retained DBN at ", dbnPath,
            " does not read back as the bytes just written. L1 is a rebuildable "
            "projection of these files (SPEC D6); a file that does not read back "
            "is not a baseline."));
      }
    }
    st.rep.dbnBytes = dbnBytes.size();

    st.sessionId = opts.sessionId != 0 ? opts.sessionId : deriveSessionId(dbnBytes);
    st.rep.sessionId = st.sessionId;

    // ---- decode twice, at two chunkings, and compare the RENDERED rows ---
    ARROW_ASSIGN_OR_RAISE(auto whole,
                          decodeDbn(dbnBytes, st.sessionId, dbnBytes.size()));
    // 8191: a chunk that lands inside the 100-byte fixed metadata block, inside
    // the variable symbology section and inside records of both 56 and 48
    // bytes, so no boundary in the file is also a feed boundary.
    ARROW_ASSIGN_OR_RAISE(const auto chunked, decodeDbn(dbnBytes, st.sessionId, 8191));

    std::ostringstream note;
    bool ok = true;
    const std::string rowsWhole   = renderRows(whole);
    const std::string rowsChunked = renderRows(chunked);
    if (rowsWhole != rowsChunked) {
      ok = false;
      note << "the retained DBN decodes to DIFFERENT canonical rows depending on "
              "the feed chunking (whole file vs 8191-byte chunks): "
           << whole.ticks.size() << "/" << whole.book.size() << " vs "
           << chunked.ticks.size() << "/" << chunked.book.size()
           << " tick/book records. ";
    }

    // The second axis, and the stronger one: the canonical rows must carry the
    // (ts_event, provider_seq, price) the CSV carried, for every record that
    // produces a row. This is what shows the CSV -> DBN -> canonical legs
    // LOSSLESS without a second CSV -> canonical mapping existing to compare
    // against (SPEC D1 forbids that second mapping). `Action::None` produces no
    // row by design — DBN's own "has no effect on the book" — so it is the one
    // action excluded.
    if (ok && !probes.empty()) {
      using Triple = std::tuple<std::int64_t, std::uint64_t, std::int64_t>;
      std::vector<Triple> want;
      // ⚠ CODE-REVIEW FINDING #11: the exclusion is MBO-ONLY. `onMbo` accepts
      // `Action::None` and deliberately produces no book row ("there is no
      // BookAction for 'no effect' and inventing one would put a no-op mutation
      // into the replay stream"), but `onTrade` emits a `TickRecord` for EVERY
      // `trades` record whatever its action. Excluding `N` for both schemas
      // therefore made a `trades` CSV containing one `action='N'` row fail the
      // round trip and abort a perfectly valid ingest.
      //
      // UNTRIGGERED ON THIS SAMPLE, and the near-miss is worth naming: our
      // `trades` action column is 1000/1000 `T`. The 546 `N` values are in
      // `side`, a DIFFERENT COLUMN with a different meaning ("no side specified
      // by the original source"), and conflating the two is exactly how this
      // bug would have been argued away as already covered.
      const bool skipActionNone = std::string(spec.dbnSchema) == "mbo";
      for (const auto& p : probes) {
        if (skipActionNone && p.action == static_cast<char>(dbn::Action::None)) {
          continue;
        }
        want.emplace_back(static_cast<std::int64_t>(p.tsEvent),
                          static_cast<std::uint64_t>(p.sequence),
                          p.price == dbn::kUndefPrice ? kNoPrice : p.price);
      }
      std::vector<Triple> got;
      for (const auto& t : whole.ticks) {
        got.emplace_back(t.header.tsEventNs, t.header.providerSeq, t.price);
      }
      for (const auto& b : whole.book) {
        got.emplace_back(b.header.tsEventNs, b.header.providerSeq, b.price);
      }
      std::sort(want.begin(), want.end());
      std::sort(got.begin(), got.end());
      if (want != got) {
        ok = false;
        note << "the (ts_event, provider_seq, price) multiset of the canonical "
                "rows does not match the CSV's: "
             << want.size() << " expected, " << got.size() << " produced. ";
      } else {
        note << "the (ts_event, provider_seq, price) multiset of all "
             << got.size()
             << " canonical rows matches the CSV's exactly; whole-file and "
                "8191-byte-chunk feeds of the retained DBN render identical "
                "rows";
        if (retainedMatched) {
          note << "; and re-transcoding the CSV reproduced the already-retained "
                  "DBN byte-for-byte";
        }
      }
    } else if (ok) {
      note << "whole-file and 8191-byte-chunk feeds of the retained DBN render "
              "identical rows ("
           << whole.ticks.size() << " ticks, " << whole.book.size()
           << " book events); no CSV was read, so there is no CSV cross-check "
              "on this run";
    }
    st.rep.roundTripOk   = ok;
    st.rep.roundTripNote = note.str();
    if (!ok) {
      // A FINDING (exit 1). The header's own exit-code table names the
      // round-trip mismatch as the archetypal one; `main` nonetheless reported
      // it as could-not-run until finding #1.
      return asFinding(arrow::Status::Invalid(
          "round-trip failed for ", spec.kind, ": ", st.rep.roundTripNote));
    }

    // ⚠ CODE-REVIEW FINDING #12. The insert below renders ONLY `ticks` for
    // `trades` and ONLY `book` for `book_events`, so a file that decoded into
    // the other vector inserted ZERO rows and the window was still recorded
    // `state=1` — a completeness claim over nothing, which is the one thing
    // this table must never say. Unreachable on the CSV path (the transcoder's
    // rtype gate refuses a schema/rtype mismatch) and REACHABLE on `reload`,
    // where the DBN on disk is constrained by nothing at all: rename
    // `*.trades.dbn` over `*.mbo.dbn` and the mbo kind decodes 1000 ticks, zero
    // book events, and claims the window.
    {
      const bool wantTicks = std::string(spec.kind) == "trades";
      const std::size_t wanted = wantTicks ? whole.ticks.size() : whole.book.size();
      const std::size_t other  = wantTicks ? whole.book.size() : whole.ticks.size();
      if (other != 0 || wanted == 0) {
        return asFinding(arrow::Status::Invalid(
            "the DBN for kind `", spec.kind, "` decoded into ",
            whole.ticks.size(), " tick record(s) and ", whole.book.size(),
            " book record(s), but gma.", spec.table, " holds only the ",
            wantTicks ? "tick" : "book",
            " kind. Inserting the matching half and recording the window "
            "complete would assert coverage over rows that were silently "
            "dropped (SPEC D11). Check that ", dbnPath,
            " really is the `", spec.dbnSchema, "` pull."));
      }
    }

    st.rep.decodedRecords  = whole.stats.recordsIn;
    st.rep.canonicalRows   = whole.ticks.size() + whole.book.size();

    // ENC-1484's number, free while the records are in hand and measured HERE
    // rather than beside the insert so `--no-insert` reports it too: D5 picked
    // plain MergeTree because (symbol, ts_event, provider_seq) is NOT unique,
    // and this is by how much. One symbol, so the symbol leg is constant.
    {
      std::vector<std::pair<std::int64_t, std::uint64_t>> keys;
      keys.reserve(st.rep.canonicalRows);
      for (const auto& t : whole.ticks) {
        keys.emplace_back(t.header.tsEventNs, t.header.providerSeq);
      }
      for (const auto& b2 : whole.book) {
        keys.emplace_back(b2.header.tsEventNs, b2.header.providerSeq);
      }
      std::sort(keys.begin(), keys.end());
      const auto last = std::unique(keys.begin(), keys.end());
      st.rep.distinctSortKeys = static_cast<std::size_t>(last - keys.begin());
      st.rep.duplicateSortKeys = keys.size() - st.rep.distinctSortKeys;
    }
    st.rep.sideUnspecified = whole.stats.sideUnspecified;
    st.rep.symbol          = whole.symbol;
    st.rep.venue           = whole.venue;
    st.rep.instrumentId    = whole.instrumentId;
    st.dataset             = whole.meta.dataset;

    if (opts.fromDbnOnly) {
      // The window from the DBN's own metadata — the whole point of writing
      // `limit` into it.
      st.rep.pullLimit       = whole.meta.limit;
      st.rep.requestedFromNs = static_cast<std::int64_t>(whole.meta.start);
      st.rep.requestedToNs   = static_cast<std::int64_t>(whole.meta.end);
      st.rep.truncatedByLimit =
          whole.meta.limit != 0 && whole.stats.recordsIn == whole.meta.limit;
      std::int64_t lastTs = 0;
      for (const auto& t : whole.ticks) lastTs = std::max(lastTs, t.header.tsEventNs);
      for (const auto& b : whole.book) lastTs = std::max(lastTs, b.header.tsEventNs);
      st.rep.recordedFromNs = st.rep.requestedFromNs;
      st.rep.recordedToNs = st.rep.truncatedByLimit ? lastTs : st.rep.requestedToNs;
      for (const auto& m : whole.meta.mappings) {
        for (const auto& iv : m.intervals) {
          std::uint32_t id = 0;
          if (!parseExact(iv.symbol, id)) continue;  // not an id in this interval
          st.mappings.push_back(csv::Transcoded::Mapping{
              id, m.rawSymbol, iv.startDate,
              iv.endDate > iv.startDate ? iv.endDate - 1 : iv.startDate});
        }
      }
    }

    if (st.rep.recordedToNs <= st.rep.recordedFromNs) {
      return asFinding(arrow::Status::Invalid(
          "the delivered window for ", spec.kind, " came out empty (",
          formatWindow(st.rep.recordedFromNs, st.rep.recordedToNs),
          "). Writing it as an ingested window would assert coverage of "
          "nothing."));
    }

    st.decoded = std::move(whole);
    staged.push_back(std::move(st));
  }

  if (opts.noInsert) {
    for (const auto& st : staged) rep.kinds.push_back(st.rep);
    return rep;
  }

  // ---- pass 2: drop, insert, record -------------------------------------
  for (auto& st : staged) {
    // Day partitions this kind's rows land in, plus the recorded window's own
    // days: a window may claim a day the rows do not reach (a quiet tail), and
    // that day's partition is still being asserted over.
    std::set<std::uint32_t> days = daysOf(st.rep.recordedFromNs, st.rep.recordedToNs);
    for (const auto& t : st.decoded.ticks) {
      days.insert(csv::yyyymmddUtc(static_cast<std::uint64_t>(t.header.tsEventNs)));
    }
    for (const auto& b : st.decoded.book) {
      days.insert(csv::yyyymmddUtc(static_cast<std::uint64_t>(b.header.tsEventNs)));
    }

    // ---- THE INVARIANT THE SCHEMA CANNOT ENFORCE -----------------------
    //
    // From the warning block in clickhouse.sql: a DROP PARTITION invalidates
    // EVERY symbol's completeness rows for that partition, not only the symbol
    // being re-ingested, because partitions are day-granular while completeness
    // is per symbol. Without this step another symbol's newest row still reads
    // state=1 over an emptied partition, and isComplete returns true over
    // nothing.
    //
    // So before dropping: every OTHER symbol with a recorded window for this
    // kind intersecting a partition about to go gets a state=3 (invalidated)
    // row. Our own symbol gets the state=0 below, which covers it.
    for (const std::uint32_t day : days) {
      const auto dayFrom = static_cast<std::int64_t>(csv::midnightUtcNs(day));
      const std::int64_t dayTo = dayFrom + 86400000000000LL;
      std::ostringstream q;
      q << "SELECT DISTINCT symbol, toUnixTimestamp64Nano(from_ts), "
           "toUnixTimestamp64Nano(to_ts) FROM gma.ingest_windows WHERE kind = "
        << ch::sqlQuote(st.rep.kind) << " AND symbol != "
        << ch::sqlQuote(st.rep.symbol)
        << " AND from_ts < fromUnixTimestamp64Nano(toInt64(" << dayTo
        << "), 'UTC') AND to_ts > fromUnixTimestamp64Nano(toInt64(" << dayFrom
        << "), 'UTC') FORMAT TabSeparated";
      ARROW_ASSIGN_OR_RAISE(const auto others, client.select(q.str()));
      for (const auto& r : others) {
        // ⚠ CODE-REVIEW FINDING #6. This was `if (r.size() != 3) continue;` —
        // a SILENT SKIP, inside the block whose entire stated purpose is
        // preventing `isComplete` from returning true over an emptied
        // partition. Skipping a row here means some other symbol keeps a
        // `state=1` row over data this loop is about to drop, which is the
        // exact failure the block exists to prevent, reached by the code meant
        // to prevent it. It refuses now.
        if (r.size() != 3) {
          return arrow::Status::Invalid(
              "a gma.ingest_windows row came back with ", r.size(),
              " field(s) instead of 3 while collecting the other symbols whose "
              "completeness this DROP PARTITION invalidates. Refusing rather "
              "than skipping: a skipped row leaves another symbol asserting "
              "state=1 over a partition about to be emptied. Nothing has been "
              "dropped yet.");
        }
        std::int64_t otherFrom = 0;
        std::int64_t otherTo   = 0;
        if (!parseExact(r[1], otherFrom)) return badField("from_ts", r[1]);
        if (!parseExact(r[2], otherTo)) return badField("to_ts", r[2]);
        ARROW_RETURN_NOT_OK(writeWindowRow(
            client, r[0], st.rep.kind, otherFrom, otherTo,
            /*state=*/3, /*rowsInserted=*/0, st.sessionId,
            "invalidated by DROP PARTITION " + std::to_string(day) + " for " +
                st.rep.symbol));
        rep.invalidationRows.push_back(r[0] + "/" + st.rep.kind + " " + r[1] +
                                       ".." + r[2] + " -> state=3 (partition " +
                                       std::to_string(day) + ")");
      }
    }

    // ---- state=0, in flight --------------------------------------------
    ARROW_RETURN_NOT_OK(writeWindowRow(client, st.rep.symbol, st.rep.kind,
                                       st.rep.recordedFromNs, st.rep.recordedToNs,
                                       /*state=*/0, /*rowsInserted=*/0,
                                       st.sessionId, st.rep.jobId));

    // ---- DROP PARTITION, then reload (D5) ------------------------------
    for (const std::uint32_t day : days) {
      std::ostringstream q;
      q << "ALTER TABLE gma." << st.table << " DROP PARTITION " << day;
      ARROW_RETURN_NOT_OK(client.exec(q.str()).status());
      rep.partitionsDropped.push_back(st.table + ":" + std::to_string(day));
    }

    // ---- sort by the physical sort key, then insert ---------------------
    //
    // docs 8.6: "Insert symbol-sorted batches per day partition — the key leads
    // with symbol, so raw DBN file order (time-ordered) forces a per-part
    // re-sort. The ingest job owns this; a background merge cannot fix it after
    // the fact." At one symbol and one day this sort is the identity, which is
    // exactly why it goes in now: getting it wrong is invisible here and
    // expensive at the universe.
    struct Row {
      std::int64_t  tsEventNs = 0;
      std::uint64_t providerSeq = 0;
      std::string   text;
    };
    std::vector<Row> rows;
    if (st.rep.kind == "trades") {
      for (const auto& t : st.decoded.ticks) {
        rows.push_back(Row{t.header.tsEventNs, t.header.providerSeq,
                           renderTradeRow(t, st.decoded.symbol, st.decoded.venue)});
      }
    } else {
      for (const auto& b : st.decoded.book) {
        rows.push_back(Row{b.header.tsEventNs, b.header.providerSeq,
                           renderBookRow(b, st.decoded.symbol, st.decoded.venue)});
      }
    }
    // One symbol, so the symbol leg of (symbol, ts_event, provider_seq) is
    // constant and `stable_sort` on the remaining two is the same order — with
    // ties left in arrival order, which matters because the key is NOT unique.
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
      return a.tsEventNs != b.tsEventNs ? a.tsEventNs < b.tsEventNs
                                        : a.providerSeq < b.providerSeq;
    });

    std::string body = "INSERT INTO gma.";
    body += st.table;
    body += " (";
    body += st.rep.kind == "trades" ? kTradesColumns : kBookEventsColumns;
    body += ") FORMAT TabSeparated\n";
    for (const auto& r : rows) body += r.text;

    const arrow::Status ins =
        rows.empty() ? arrow::Status::OK() : client.exec(body).status();
    if (!ins.ok()) {
      // state=2, failed. Written before returning so the window never reads as
      // anything but unloaded: the state=0 row above is already the newest, and
      // this makes the failure explicit rather than leaving it in flight
      // forever.
      ARROW_RETURN_NOT_OK(writeWindowRow(
          client, st.rep.symbol, st.rep.kind, st.rep.recordedFromNs,
          st.rep.recordedToNs, /*state=*/2, 0, st.sessionId, st.rep.jobId));
      return ins;
    }
    st.rep.rowsInserted = rows.size();

    // ---- symbology (SPEC D7, docs 8.9) ---------------------------------
    for (const auto& m : st.mappings) {
      const std::uint64_t validFrom = csv::midnightUtcNs(m.firstDay);
      // HALF-OPEN [valid_from, valid_to): valid_to is midnight of the day AFTER
      // the last day the mapping was observed on.
      const std::uint64_t validTo =
          csv::midnightUtcNs(m.lastDay) + 86400000000000ULL;
      std::ostringstream q;
      // `dataset` and `raw_symbol` come out of the vendor DBN's own metadata,
      // and `source_ref` is either a vendor job id or a filesystem path — all
      // three are quoted rather than concatenated (finding #5).
      q << "INSERT INTO gma.symbology (dataset, instrument_id, raw_symbol, "
           "valid_from, valid_to, source_ref, ingested_at) SELECT "
        << ch::sqlQuote(st.dataset) << ", " << m.instrumentId << ", "
        << ch::sqlQuote(m.rawSymbol) << ", fromUnixTimestamp64Nano(toInt64("
        << validFrom << "), 'UTC'), fromUnixTimestamp64Nano(toInt64(" << validTo
        << "), 'UTC'), "
        << ch::sqlQuote(st.rep.jobId.empty() ? st.rep.dbnPath : st.rep.jobId)
        << ", now64(3)";
      ARROW_RETURN_NOT_OK(client.exec(q.str()).status());
      ++rep.symbologyRows;
    }

    // ---- state=1, complete ---------------------------------------------
    ARROW_RETURN_NOT_OK(writeWindowRow(client, st.rep.symbol, st.rep.kind,
                                       st.rep.recordedFromNs, st.rep.recordedToNs,
                                       /*state=*/1, rows.size(), st.sessionId,
                                       st.rep.jobId));
    rep.kinds.push_back(st.rep);
  }

  return rep;
}

}  // namespace gma::data::ingest
