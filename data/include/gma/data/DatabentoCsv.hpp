#pragma once
//
// Databento CSV -> the vendor's own PODs -> real DBN bytes (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D6, D7, D8;
//         docs/data-layer.md §8.6, §8.7, §8.8.
//
// WHY THIS STOPS AT `dbn::MboMsg` / `dbn::TradeMsg` AND NOT AT A `BookRecord`
// --------------------------------------------------------------------------
// The sample Nick pulled is CSV (`encoding: csv`, `pretty_px: false`,
// `pretty_ts: false`), and the canonical adapter is a DBN decoder. The
// tempting shortcut is a CSV -> canonical mapping, and SPEC D1 refuses it: one
// mapping from provider semantics to canonical records exists in this system
// and it is `DatabentoAdapter`'s. A second one would be a second place for the
// Fill/Cancel question, the aggressor flip and the `Side::None` encoding to be
// answered, and the two answers would differ silently.
//
// So this file translates ONE VENDOR ENCODING INTO ANOTHER and then stops. Its
// output type is the vendor's own 56-/48-byte POD, exactly as the vendor's
// binary would have decoded to. `DbnEncoder` turns those into DBN bytes, and
// those bytes are what `DatabentoAdapter` decodes. The encoder's fidelity is
// established byte-for-byte against the five real vendor DBN fixtures
// (`DbnEncoderTest.EncoderReproducesEveryFixtureByteForByte`), which is what
// makes this route sound rather than circular: the DBN we hand the adapter is
// what Databento would have sent for these values.
//
// COLUMNS ARE RESOLVED BY NAME, NEVER BY POSITION, and that is not defensive
// tidiness. The ticket describing this work said the `trades` schema
// "substitutes `depth` for `channel_id`/`order_id`"; what the file actually
// carries is
//
//   mbo     ts_recv ts_event rtype publisher_id instrument_id action side
//           price size channel_id order_id flags ts_in_delta sequence symbol
//   trades  ts_recv ts_event rtype publisher_id instrument_id action side
//           depth price size flags ts_in_delta sequence symbol
//
// — `depth` sits BETWEEN `side` AND `price`, so a positional reader built from
// that description would have read `depth` as `price` and `price` as `size`
// and produced a table of 0-priced trades. A missing required column is
// refused by name instead.
//
// THE SAMPLE IS LICENSED VENDOR DATA AND LIVES OUTSIDE EVERY GIT WORKING TREE
// (`~/market-data/databento/...`), because `docs/data-layer.md` §14.2 names
// committing it a redistribution question not worth answering. Nothing here
// embeds a row of it; the path is a parameter with that default.
//

#include "gma/data/DatabentoAdapter.hpp"

#include <arrow/result.h>
#include <arrow/status.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gma::data::csv {

// ---------------------------------------------------------------------------
// The vendor's batch-job metadata, read from `metadata.json`.
// ---------------------------------------------------------------------------
//
// Read rather than assumed because three of its fields are load-bearing and
// none of them is recoverable from the CSV rows:
//
//   * `start`/`end` are the REQUESTED window, which is what
//     `gma.ingest_windows` has an opinion about. The first row's `ts_event` is
//     not the window start — a quiet opening minute is data, not a short pull.
//   * `limit` is how TRUNCATION is detectable at all. This sample carries
//     `limit: 1000` and delivered exactly 1000 records, so the delivered
//     window is a PREFIX of the requested one and recording the requested one
//     as complete would be the §8.8 failure in the one table that exists to
//     prevent it.
//   * `pretty_px`/`pretty_ts` decide whether `price` is an Int64 at 1e-9 (SPEC
//     D8, an assignment) or a decimal string (a conversion, i.e. a second
//     numeric path). Only the raw form is accepted; the pretty form is
//     refused by name rather than parsed.
struct PullMetadata {
  std::string jobId;    // "XNAS-20261015-9DHSFUMH59" — `source_ref` provenance
  std::string dataset;  // "XNAS.ITCH"
  std::string schema;   // "mbo" | "trades"
  std::vector<std::string> symbols;
  std::string   stypeIn;   // "raw_symbol"
  std::string   stypeOut;  // "instrument_id"
  std::uint64_t start = 0;
  std::uint64_t end   = 0;
  std::uint64_t limit = 0;  // 0 == no limit
  bool prettyPx = true;     // defaults chosen so an UNREAD field refuses
  bool prettyTs = true;
};

arrow::Result<PullMetadata> readPullMetadata(const std::string& path);

// ---------------------------------------------------------------------------
// The result of transcoding one CSV.
// ---------------------------------------------------------------------------
struct Transcoded {
  std::vector<std::byte> dbn;  // real DBN bytes, ready for DatabentoAdapter

  std::size_t   records      = 0;
  std::uint64_t firstTsEvent = 0;
  std::uint64_t lastTsEvent  = 0;
  // How many records carry `lastTsEvent`. Load-bearing on a truncated pull:
  // if the cut fell inside a group of records sharing one instant, that
  // instant is PARTIALLY delivered, and nothing in the file says whether it
  // did. See `deliveredEnd()`.
  std::size_t   recordsAtLastTsEvent = 0;

  // `limit != 0 && records == limit`. The vendor stops at `limit` records in
  // time order, so a pull that returns exactly its limit is a prefix of the
  // requested window and its true end is unknown.
  bool truncatedByLimit = false;

  // The (instrument_id, raw_symbol, day) triples observed, which is where the
  // DBN metadata's symbology section comes from. With `map_symbols: true` the
  // CSV's `symbol` column IS the vendor's mapping carrier.
  struct Mapping {
    std::uint32_t instrumentId = 0;
    std::string   rawSymbol;
    std::uint32_t firstDay = 0;  // yyyymmdd
    std::uint32_t lastDay  = 0;  // yyyymmdd, INCLUSIVE; the DBN interval is
                                 // half-open [firstDay, lastDay + 1)
  };
  std::vector<Mapping> mappings;

  // Per-action record counts, keyed by the DBN action character. Reported
  // rather than summarized: "no `M` and no `R`" is a statement about what this
  // data does NOT exercise, and it has to be visible to be said honestly.
  std::vector<std::pair<char, std::size_t>> actionCounts;

  // (ts_event, sequence, price) of every record, in file order. The
  // round-trip assertion compares this against what the ADAPTER produced from
  // the DBN, which is how the encode/decode leg is shown lossless without a
  // second CSV -> canonical mapping existing to compare against.
  struct Probe {
    std::uint64_t tsEvent = 0;
    std::uint32_t sequence = 0;
    std::int64_t  price = 0;
    char          action = 0;
  };
  std::vector<Probe> probes;

  // `ts_recv == ts_event + ts_in_delta` holds on every row. Counted rather
  // than assumed: it is the fact that confines `ts_recv` to diagnostics
  // (docs §8.3) by making it derivable.
  std::size_t tsRecvMatchesDelta = 0;

  // The half-open end of what the file ACTUALLY delivers, in ns.
  //
  //   not truncated -> the requested `end`: the vendor delivered everything in
  //                    [start, end), so the whole window is covered even if
  //                    the last record is earlier.
  //   truncated     -> `lastTsEvent`, EXCLUSIVE. Not `lastTsEvent + 1`: the
  //                    cut may have fallen inside a group of records sharing
  //                    that instant (this sample's mbo file has ten records at
  //                    its last `ts_event`), and the file cannot say whether it
  //                    did. Dropping the possibly-partial final instant from
  //                    the CLAIM is the conservative direction; the rows
  //                    themselves are still inserted.
  std::uint64_t deliveredEnd(const PullMetadata& md) const {
    return md.end;
  }
};

// ---------------------------------------------------------------------------
// Transcode.
// ---------------------------------------------------------------------------
//
// `dbnVersion` is the DBN version to EMIT. v3 is the default because it is
// what a batch job delivers today, and because it is the version that pads its
// metadata to an 8-byte boundary — the detail ENC-1482 found by measurement
// and the one a decoder reading records at a 7-byte offset gets wrong. Writing
// v3 therefore drives that path on every ingest rather than only in a test.
arrow::Result<Transcoded> transcodeMboCsv(const std::string& csvPath,
                                          const PullMetadata& md,
                                          std::uint8_t dbnVersion = 3);
arrow::Result<Transcoded> transcodeTradesCsv(const std::string& csvPath,
                                             const PullMetadata& md,
                                             std::uint8_t dbnVersion = 3);

// Dispatches on `md.schema`; refuses any other schema by name rather than
// guessing a record layout.
arrow::Result<Transcoded> transcodeCsv(const std::string& csvPath,
                                       const PullMetadata& md,
                                       std::uint8_t dbnVersion = 3);

// ---------------------------------------------------------------------------
// Retention (SPEC D6).
// ---------------------------------------------------------------------------
//
// Writes `bytes` to `path` and returns whether anything changed. D6 makes the
// raw files load-bearing — D5's re-ingest is `DROP PARTITION` + reload, and a
// reload must reload FROM something — so the ingest job writes the DBN once
// and reads it back on every subsequent run.
//
// An existing file with identical bytes is left alone and reported as
// unchanged, which is also the CSV -> DBN determinism check: a second
// transcode of the same CSV that produced different bytes would show up here
// as a rewrite rather than passing unnoticed.
struct RetentionResult {
  bool existed = false;
  bool changed = false;
  std::size_t bytes = 0;
};
arrow::Result<RetentionResult> retain(const std::string& path,
                                      const std::vector<std::byte>& bytes);

arrow::Result<std::vector<std::byte>> readFile(const std::string& path);

// yyyymmdd (UTC) for a nanosecond timestamp — the form DBN's symbology
// intervals use.
std::uint32_t yyyymmddUtc(std::uint64_t tsNs);
// Midnight UTC of a yyyymmdd, in ns. The inverse, for `gma.symbology`'s
// half-open `valid_from`/`valid_to`.
std::uint64_t midnightUtcNs(std::uint32_t yyyymmdd);

}  // namespace gma::data::csv
