//
// Databento CSV -> vendor PODs -> DBN (ENC-1483).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D6, D8;
//         docs/data-layer.md §8.7, §8.8.
//
// NO ROW OF THE REAL SAMPLE APPEARS HERE. The sample is licensed vendor data
// living outside every git working tree (docs/data-layer.md §14.2 names
// committing Databento data a redistribution question not worth answering), so
// every CSV below is INVENTED. That costs nothing, because what these cases
// test is the parser's refusals and the shape of the window arithmetic — not
// the contents of a particular file. The real file is driven by
// `gma_ingest ingest`, whose exit code is the verdict.
//
// THE REAL SAMPLE'S OWN NUMBERS LIVE IN THE TICKET AND IN THE COMMIT MESSAGE,
// not in an assertion here, for the same reason: a test asserting "1000
// records" would be a test about a file nobody can check out.
//

#include "gma/data/DatabentoCsv.hpp"

#include "gma/data/DatabentoAdapter.hpp"
#include "gma/data/IngestJob.hpp"
#include "gma/data/SymbolTable.hpp"
#include "gma/data/Verdict.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace gma::data::csv {
namespace {

// A per-test temp directory. NOT a fixed name under /tmp: parallel agents share
// one scratchpad and one /tmp, and two runs writing the same path corrupt each
// other's artifact — measured, ENC-1183/ENC-1185, where the corrupted artifact
// was a gate-verification table. `unique_path` per test, removed after.
class TempDir {
 public:
  TempDir() {
    for (int attempt = 0;; ++attempt) {
      path_ = std::filesystem::temp_directory_path() /
              ("ENC-1483-csvtest-" + std::to_string(::getpid()) + "-" +
               std::to_string(counter_++) + "-" + std::to_string(attempt));
      std::error_code ec;
      if (std::filesystem::create_directories(path_, ec)) return;
      if (attempt > 16) {
        ADD_FAILURE() << "cannot create a temp directory: " << ec.message();
        return;
      }
    }
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::string file(const std::string& name, const std::string& content) const {
    const auto p = path_ / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
    return p.string();
  }
  std::string at(const std::string& name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
  static int counter_;
};
int TempDir::counter_ = 0;

// 2022-06-10T12:30:01.000000000Z, and two instants after it.
constexpr std::uint64_t kT0 = 1654864201000000000ULL;
constexpr std::uint64_t kDayStart = 1654819200000000000ULL;  // 2022-06-10T00:00Z
constexpr std::uint64_t kNextDay  = 1654905600000000000ULL;  // 2022-06-11T00:00Z

PullMetadata md(const std::string& schema, std::uint64_t limit = 0) {
  PullMetadata m;
  m.jobId    = "TEST-JOB";
  m.dataset  = "XNAS.ITCH";
  m.schema   = schema;
  m.symbols  = {"ZZZZ"};
  m.stypeIn  = "raw_symbol";
  m.stypeOut = "instrument_id";
  m.start    = kDayStart + 45000000000000ULL;  // 12:30:00
  m.end      = kDayStart + 50400000000000ULL;  // 14:00:00
  m.limit    = limit;
  m.prettyPx = false;
  m.prettyTs = false;
  return m;
}

const char* kMboHeader =
    "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,price,size,"
    "channel_id,order_id,flags,ts_in_delta,sequence,symbol\n";

// One mbo row. `rtype` 160 is dbn::RType::Mbo.
std::string mboRow(std::uint64_t tsEvent, char action, char side,
                   std::int64_t price, std::uint32_t size, std::uint64_t orderId,
                   std::uint32_t sequence, int tsInDelta = 11) {
  return std::to_string(tsEvent + static_cast<std::uint64_t>(tsInDelta)) + "," +
         std::to_string(tsEvent) + ",160,2,4242," + std::string(1, action) + "," +
         std::string(1, side) + "," + std::to_string(price) + "," +
         std::to_string(size) + ",0," + std::to_string(orderId) + ",0," +
         std::to_string(tsInDelta) + "," + std::to_string(sequence) + ",ZZZZ\n";
}

const char* kTradesHeader =
    "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,depth,price,"
    "size,flags,ts_in_delta,sequence,symbol\n";

// `depth` sits BETWEEN `side` AND `price` — the layout the ticket's description
// got wrong and the reason columns are resolved by name.
std::string tradeRow(std::uint64_t tsEvent, char side, std::int64_t price,
                     std::uint32_t size, std::uint32_t sequence, int tsInDelta = 13) {
  return std::to_string(tsEvent + static_cast<std::uint64_t>(tsInDelta)) + "," +
         std::to_string(tsEvent) + ",0,2,4242,T," + std::string(1, side) + ",0," +
         std::to_string(price) + "," + std::to_string(size) + ",0," +
         std::to_string(tsInDelta) + "," + std::to_string(sequence) + ",ZZZZ\n";
}

// ===========================================================================
// Dates
// ===========================================================================

TEST(DatabentoCsvDates, YyyymmddUtcAndMidnightAreInverses) {
  EXPECT_EQ(yyyymmddUtc(kT0), 20220610u);
  EXPECT_EQ(midnightUtcNs(20220610), kDayStart);
  EXPECT_EQ(midnightUtcNs(20220611), kNextDay);
  // The boundary, both sides of it: a nanosecond before midnight is the
  // previous day. Getting this wrong puts a symbology interval or a partition
  // one day out.
  EXPECT_EQ(yyyymmddUtc(kNextDay - 1), 20220610u);
  EXPECT_EQ(yyyymmddUtc(kNextDay), 20220611u);
  // A leap day and a century boundary, since the arithmetic is hand-rolled.
  EXPECT_EQ(yyyymmddUtc(midnightUtcNs(20240229)), 20240229u);
  EXPECT_EQ(yyyymmddUtc(midnightUtcNs(20000301)), 20000301u);
  EXPECT_EQ(yyyymmddUtc(midnightUtcNs(19700101)), 19700101u);
}

// ===========================================================================
// metadata.json
// ===========================================================================

std::string metadataJson(const std::string& schema, const char* prettyPx,
                         const char* prettyTs, std::uint64_t limit = 1000) {
  return std::string(R"({"version":1,"job_id":"TEST-JOB","query":{)") +
         R"("dataset":"XNAS.ITCH","schema":")" + schema +
         R"(","symbols":["ZZZZ"],"stype_in":"raw_symbol",)" +
         R"("stype_out":"instrument_id","start":1654864200000000000,)" +
         R"("end":1654869600000000000,"limit":)" + std::to_string(limit) +
         R"(,"encoding":"csv"},"customizations":{"pretty_px":)" + prettyPx +
         R"(,"pretty_ts":)" + prettyTs + R"(,"map_symbols":true}})";
}

TEST(DatabentoCsvMetadata, TheRawFormIsReadAndTheRequestedWindowIsTheQuerys) {
  const TempDir d;
  const auto p = d.file("metadata.json", metadataJson("mbo", "false", "false"));
  const auto r = readPullMetadata(p);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->jobId, "TEST-JOB");
  EXPECT_EQ(r->dataset, "XNAS.ITCH");
  EXPECT_EQ(r->schema, "mbo");
  EXPECT_EQ(r->start, 1654864200000000000ULL);
  EXPECT_EQ(r->end, 1654869600000000000ULL);
  EXPECT_EQ(r->limit, 1000u);
  ASSERT_EQ(r->symbols.size(), 1u);
  EXPECT_EQ(r->symbols[0], "ZZZZ");
}

TEST(DatabentoCsvMetadata, PrettyPricesOrTimestampsAreRefusedNotConverted) {
  const TempDir d;
  // SPEC D8 is "an assignment, not a conversion". A pretty CSV carries
  // `265.76` and an ISO timestamp, and reading those would introduce a second
  // numeric representation of a price.
  for (const auto& [px, ts] : std::vector<std::pair<const char*, const char*>>{
           {"true", "false"}, {"false", "true"}, {"true", "true"}}) {
    const auto p = d.file(std::string("md-") + px + "-" + ts + ".json",
                          metadataJson("mbo", px, ts));
    const auto r = readPullMetadata(p);
    EXPECT_FALSE(r.ok()) << "pretty_px=" << px << " pretty_ts=" << ts;
    if (!r.ok()) {
      EXPECT_NE(r.status().message().find("RAW form"), std::string::npos)
          << r.status().message();
    }
  }
  // And the raw form IS accepted, so the loop above is not vacuous.
  EXPECT_TRUE(
      readPullMetadata(d.file("ok.json", metadataJson("mbo", "false", "false"))).ok());
}

TEST(DatabentoCsvMetadata, AnUnreadPrettyFieldDefaultsToRefusal) {
  const TempDir d;
  // `customizations` absent entirely. The defaults are `true`, deliberately, so
  // a metadata.json that does not SAY is refused rather than assumed raw.
  const auto p = d.file(
      "md.json",
      R"({"job_id":"J","query":{"dataset":"X","schema":"mbo","start":1,"end":2}})");
  EXPECT_FALSE(readPullMetadata(p).ok());
}

TEST(DatabentoCsvMetadata, AMissingFileIsAnErrorThatNamesWhatItIsFor) {
  const TempDir d;
  const auto r = readPullMetadata(d.at("absent.json"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("REQUESTED window"), std::string::npos)
      << r.status().message();
}

// ===========================================================================
// Columns are resolved BY NAME
// ===========================================================================

TEST(DatabentoCsvColumns, AMissingRequiredColumnIsRefusedAndNamed) {
  const TempDir d;
  // `sequence` dropped. It becomes `provider_seq`, which docs §8.3 requires
  // non-null, so a parser that tolerated its absence would synthesize L1's
  // dedup key.
  const std::string header =
      "ts_recv,ts_event,rtype,publisher_id,instrument_id,action,side,price,size,"
      "channel_id,order_id,flags,ts_in_delta,symbol\n";
  const auto p = d.file("x.csv", header);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("sequence"), std::string::npos)
      << r.status().message();
  EXPECT_NE(r.status().message().find("BY NAME"), std::string::npos)
      << r.status().message();
}

TEST(DatabentoCsvColumns, AShuffledColumnOrderParsesIdentically) {
  const TempDir d;
  // THE WHOLE POINT OF NAME RESOLUTION. The ticket describing this work said
  // the `trades` schema "substitutes depth for channel_id/order_id"; the real
  // file puts `depth` BETWEEN `side` AND `price`, so a positional reader built
  // from that description reads `depth` as `price` and ships a table of
  // zero-priced trades. Here the columns are deliberately reordered and the
  // result must be unchanged.
  const auto canonical = d.file(
      "a.csv", std::string(kTradesHeader) + tradeRow(kT0, 'B', 265760000000LL, 14, 7));
  const auto shuffled = d.file(
      "b.csv",
      "symbol,sequence,ts_in_delta,flags,size,price,depth,side,action,"
      "instrument_id,publisher_id,rtype,ts_event,ts_recv\n"
      "ZZZZ,7,13,0,14,265760000000,0,B,T,4242,2,0," +
          std::to_string(kT0) + "," + std::to_string(kT0 + 13) + "\n");

  const auto a = transcodeTradesCsv(canonical, md("trades"));
  const auto b = transcodeTradesCsv(shuffled, md("trades"));
  ASSERT_TRUE(a.ok()) << a.status().ToString();
  ASSERT_TRUE(b.ok()) << b.status().ToString();
  EXPECT_EQ(a->dbn, b->dbn) << "the DBN bytes must not depend on CSV column order";
  EXPECT_EQ(a->records, 1u);
}

TEST(DatabentoCsvColumns, AWrongFieldCountOnARowIsRefused) {
  const TempDir d;
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     "1,2,160,2,4242,A,B,100,1,0,9,0,1,5\n");
  EXPECT_FALSE(transcodeMboCsv(p, md("mbo")).ok());
}

TEST(DatabentoCsvColumns, ANonIntegerNumericFieldIsRefusedNotHalfRead) {
  const TempDir d;
  // `265.76` in a raw-price column: `strtoull` would read 265 and stop. That is
  // a plausible wrong price, which is the worst kind.
  auto row = mboRow(kT0, 'A', 'B', 100, 1, 9, 5);
  const auto pos = row.find(",100,");
  row.replace(pos, 5, ",265.76,");
  const auto p = d.file("x.csv", std::string(kMboHeader) + row);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("not an integer"), std::string::npos)
      << r.status().message();
}

// ===========================================================================
// rtype, action and side
// ===========================================================================

TEST(DatabentoCsvRefusals, AnRtypeThatIsNotTheSchemasIsRefused) {
  const TempDir d;
  // An mbo body written under rtype 0 would frame as a 56-byte record claiming
  // to be a 48-byte TradeMsg, and every field after the header would decode
  // from the wrong offset.
  auto row = mboRow(kT0, 'A', 'B', 100, 1, 9, 5);
  const auto pos = row.find(",160,");
  row.replace(pos, 5, ",0,");
  const auto p = d.file("x.csv", std::string(kMboHeader) + row);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("wrong offset"), std::string::npos)
      << r.status().message();
}

TEST(DatabentoCsvRefusals, AnUndeclaredActionOrSideIsRefused) {
  const TempDir d;
  const auto badAction =
      d.file("a.csv", std::string(kMboHeader) + mboRow(kT0, 'X', 'B', 100, 1, 9, 5));
  EXPECT_FALSE(transcodeMboCsv(badAction, md("mbo")).ok());
  const auto badSide =
      d.file("b.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'S', 100, 1, 9, 5));
  // 'S' is ITCH's spelling of sell; DBN's is 'A'. Accepting it would put the
  // wrong side in L1's `side` column, which is undetectable later.
  EXPECT_FALSE(transcodeMboCsv(badSide, md("mbo")).ok());
  // Every DECLARED action and side IS accepted, so the two refusals above are
  // not an over-wide bound.
  for (const char a : {'A', 'C', 'M', 'T', 'F', 'R', 'N'}) {
    for (const char s : {'A', 'B', 'N'}) {
      const auto p = d.file(std::string("ok-") + a + s + ".csv",
                            std::string(kMboHeader) + mboRow(kT0, a, s, 100, 1, 9, 5));
      EXPECT_TRUE(transcodeMboCsv(p, md("mbo")).ok()) << a << '/' << s;
    }
  }
}

TEST(DatabentoCsvRefusals, AMultiCharacterActionIsRefusedRatherThanTruncated) {
  const TempDir d;
  const auto p = d.file("x.csv",
                        std::string(kMboHeader) +
                            "1," + std::to_string(kT0) +
                            ",160,2,4242,AC,B,100,1,0,9,0,1,5,ZZZZ\n");
  EXPECT_FALSE(transcodeMboCsv(p, md("mbo")).ok());
}

TEST(DatabentoCsvRefusals, AnUnknownSchemaHasNoTranscoderAndSaysSo) {
  const TempDir d;
  const auto p = d.file("x.csv", kMboHeader);
  const auto r = transcodeCsv(p, md("mbp-10"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("mbp-10"), std::string::npos)
      << r.status().message();
}

TEST(DatabentoCsvRefusals, AnEmptyPullIsRefusedRatherThanRecordedAsAWindow) {
  const TempDir d;
  // Header only. Writing a gma.ingest_windows row for this would assert
  // coverage of a window with nothing in it — docs §8.8's exact failure.
  const auto p = d.file("x.csv", kMboHeader);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("ingest_windows"), std::string::npos)
      << r.status().message();
}

TEST(DatabentoCsvRefusals, TwoTickersForOneInstrumentIdInOneFileIsRefused) {
  const TempDir d;
  auto second = mboRow(kT0 + 1, 'A', 'B', 100, 1, 10, 6);
  second.replace(second.rfind("ZZZZ"), 4, "YYYY");
  const auto p =
      d.file("x.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 1, 9, 5) +
                          second);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("different tickers"), std::string::npos)
      << r.status().message();
}

// ===========================================================================
// TRUNCATION — the fact this sample turns on.
// ===========================================================================

TEST(DatabentoCsvTruncation, RecordsEqualToTheLimitMeansThePullWasCut) {
  const TempDir d;
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     mboRow(kT0, 'A', 'B', 100, 1, 9, 5) +
                                     mboRow(kT0 + 10, 'A', 'B', 101, 1, 10, 6));
  const auto m = md("mbo", /*limit=*/2);
  const auto r = transcodeMboCsv(p, m);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_TRUE(r->truncatedByLimit);
  // The delivered end is the LAST ts_event, EXCLUSIVE — not `end`, and not
  // last + 1. See the header: the cut may have fallen inside a group of records
  // sharing that instant, and nothing in the file says whether it did.
  EXPECT_EQ(r->deliveredEnd(m), kT0 + 10);
  EXPECT_LT(r->deliveredEnd(m), m.end)
      << "the requested end must NOT be claimed for a truncated pull";
}

TEST(DatabentoCsvTruncation, FewerRecordsThanTheLimitClaimsTheWholeRequest) {
  const TempDir d;
  const auto p = d.file("x.csv",
                        std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 1, 9, 5));
  const auto m = md("mbo", /*limit=*/2);
  const auto r = transcodeMboCsv(p, m);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_FALSE(r->truncatedByLimit);
  // The vendor delivered everything in [start, end), so the whole window is
  // covered even though the last record is far earlier. A quiet tail is DATA.
  EXPECT_EQ(r->deliveredEnd(m), m.end);
}

TEST(DatabentoCsvTruncation, NoLimitIsNeverTruncated) {
  const TempDir d;
  const auto p = d.file("x.csv",
                        std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 1, 9, 5));
  const auto m = md("mbo", /*limit=*/0);
  const auto r = transcodeMboCsv(p, m);
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_FALSE(r->truncatedByLimit);
  EXPECT_EQ(r->deliveredEnd(m), m.end);
}

TEST(DatabentoCsvTruncation, RecordsSharingTheLastInstantAreCounted) {
  const TempDir d;
  // Three records at the final instant. That count is what says "the cut may
  // have fallen inside this group", and it is reported rather than inferred.
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     mboRow(kT0, 'A', 'B', 100, 1, 9, 5) +
                                     mboRow(kT0 + 10, 'A', 'B', 101, 1, 10, 6) +
                                     mboRow(kT0 + 10, 'A', 'B', 102, 1, 11, 7) +
                                     mboRow(kT0 + 10, 'A', 'B', 103, 1, 12, 8));
  const auto r = transcodeMboCsv(p, md("mbo", 4));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->recordsAtLastTsEvent, 3u);
  EXPECT_EQ(r->firstTsEvent, kT0);
  EXPECT_EQ(r->lastTsEvent, kT0 + 10);
}

// ===========================================================================
// ts_recv == ts_event + ts_in_delta, and the action census
// ===========================================================================

TEST(DatabentoCsvFacts, TsRecvDerivabilityIsCountedNotAssumed) {
  const TempDir d;
  auto broken = mboRow(kT0 + 10, 'A', 'B', 101, 1, 10, 6);
  broken.replace(0, broken.find(','), "999");  // ts_recv no longer derivable
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     mboRow(kT0, 'A', 'B', 100, 1, 9, 5) + broken);
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->records, 2u);
  EXPECT_EQ(r->tsRecvMatchesDelta, 1u)
      << "the identity is a COUNT, so 'holds on every row' is a measurement "
         "rather than a claim";
}

TEST(DatabentoCsvFacts, TheActionCensusIsReportedPerAction) {
  const TempDir d;
  const auto p = d.file(
      "x.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 10, 9, 5) +
                   mboRow(kT0 + 1, 'A', 'B', 100, 10, 10, 6) +
                   mboRow(kT0 + 2, 'C', 'B', 100, 10, 9, 7));
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  ASSERT_EQ(r->actionCounts.size(), 2u);
  EXPECT_EQ(r->actionCounts[0].first, 'A');
  EXPECT_EQ(r->actionCounts[0].second, 2u);
  EXPECT_EQ(r->actionCounts[1].first, 'C');
  EXPECT_EQ(r->actionCounts[1].second, 1u);
}

// ===========================================================================
// THE JOIN: the transcoded bytes are real DBN, and the adapter decodes them.
// ===========================================================================

TEST(DatabentoCsvTranscode, TheOutputDecodesThroughTheRealAdapter) {
  const TempDir d;
  const auto p = d.file(
      "x.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 265760000000LL, 10, 77, 5) +
                   mboRow(kT0 + 1, 'C', 'B', 265760000000LL, 4, 77, 6) +
                   mboRow(kT0 + 2, 'T', 'A', 265700000000LL, 2, 0, 7));
  const auto t = transcodeMboCsv(p, md("mbo"));
  ASSERT_TRUE(t.ok()) << t.status().ToString();

  SymbolTable symbols;
  SymbolTable venues;
  DatabentoAdapter::Config cfg;
  cfg.sessionId = 4242;
  DatabentoAdapter adapter(cfg, symbols, venues);
  ASSERT_TRUE(adapter.feed(std::span<const std::byte>(t->dbn.data(), t->dbn.size())).ok());
  ASSERT_TRUE(adapter.finish().ok());

  // The symbology harvested from the CSV's `symbol` column reaches the adapter
  // through the DBN metadata and resolves the ticker — SPEC D7, and the reason
  // the mapping is written into the DBN at all rather than asserted by us.
  ASSERT_TRUE(adapter.metadata().has_value());
  EXPECT_EQ(adapter.metadata()->dataset, "XNAS.ITCH");
  EXPECT_EQ(adapter.metadata()->limit, 0u);

  const auto& book = adapter.bookEvents();
  ASSERT_EQ(book.size(), 3u);
  EXPECT_EQ(symbols.name(book[0].header.symbolId), "ZZZZ");
  EXPECT_EQ(venues.name(book[0].header.venueId), "XNAS.ITCH");
  EXPECT_EQ(book[0].header.instrumentId, 4242u);
  EXPECT_EQ(book[0].header.sessionId, 4242u);

  EXPECT_EQ(book[0].action, BookAction::Add);
  EXPECT_EQ(book[0].price, 265760000000LL);
  EXPECT_EQ(book[0].size, 10u);
  EXPECT_EQ(book[0].orderId, 77u);
  EXPECT_EQ(book[0].header.providerSeq, 5u);
  EXPECT_EQ(book[0].header.tsEventNs, static_cast<std::int64_t>(kT0));
  EXPECT_EQ(book[0].header.tsRecvNs, static_cast<std::int64_t>(kT0 + 11));

  // A partial cancel resolves against the tracked resting size: 10 - 4 = 6.
  EXPECT_EQ(book[1].action, BookAction::Update);
  EXPECT_EQ(book[1].size, 6u);

  EXPECT_EQ(book[2].action, BookAction::Trade);
  EXPECT_EQ(book[2].side, Side::Ask) << "a `trades`-style T names the aggressor";
}

TEST(DatabentoCsvTranscode, ATradesCsvDecodesToBookNeutralTicks) {
  const TempDir d;
  const auto p = d.file("x.csv", std::string(kTradesHeader) +
                                     tradeRow(kT0, 'A', 265760000000LL, 14, 5) +
                                     tradeRow(kT0 + 1, 'N', 265700000000LL, 2, 6));
  const auto t = transcodeTradesCsv(p, md("trades"));
  ASSERT_TRUE(t.ok()) << t.status().ToString();

  SymbolTable symbols;
  SymbolTable venues;
  DatabentoAdapter adapter(DatabentoAdapter::Config{}, symbols, venues);
  ASSERT_TRUE(adapter.feed(std::span<const std::byte>(t->dbn.data(), t->dbn.size())).ok());
  ASSERT_TRUE(adapter.finish().ok());

  const auto& ticks = adapter.trades();
  ASSERT_EQ(ticks.size(), 2u);
  EXPECT_EQ(ticks[0].aggressor, Aggressor::Sell) << "DBN 'A' is a sell aggressor";
  EXPECT_EQ(ticks[0].price, 265760000000LL);
  EXPECT_EQ(ticks[0].size, 14u);
  // SPEC D12 / docs C13: every `trades` row is book-neutral, because the same
  // execution is also in `book_events`.
  EXPECT_TRUE(ticks[0].bookNeutral);
  EXPECT_TRUE(ticks[1].bookNeutral);
  EXPECT_EQ(ticks[1].aggressor, Aggressor::Unknown) << "DBN side 'N'";
}

TEST(DatabentoCsvTranscode, EveryDbnVersionTheAdapterAcceptsRoundTrips) {
  const TempDir d;
  const auto p = d.file(
      "x.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 10, 9, 5));
  for (std::uint8_t v = dbn::kMinVersion; v <= dbn::kMaxVersion; ++v) {
    SCOPED_TRACE("DBN v" + std::to_string(static_cast<unsigned>(v)));
    const auto t = transcodeMboCsv(p, md("mbo"), v);
    ASSERT_TRUE(t.ok()) << t.status().ToString();
    SymbolTable symbols;
    SymbolTable venues;
    DatabentoAdapter adapter(DatabentoAdapter::Config{}, symbols, venues);
    ASSERT_TRUE(
        adapter.feed(std::span<const std::byte>(t->dbn.data(), t->dbn.size())).ok());
    ASSERT_TRUE(adapter.finish().ok());
    ASSERT_EQ(adapter.bookEvents().size(), 1u);
    ASSERT_TRUE(adapter.metadata().has_value());
    EXPECT_EQ(adapter.metadata()->version, v);
    // v3 pads its metadata to an 8-byte boundary and v1/v2 do not — the detail
    // ENC-1482 found by measurement, and the one that makes a decoder read
    // every record at a 7-byte offset. The ingest job emits v3 by default
    // precisely so this path runs on every real ingest.
    if (v < dbn::kFirstAlignedVersion) {
      EXPECT_EQ(adapter.metadata()->endPadding, 0u);
    }
  }
}

TEST(DatabentoCsvTranscode, FeedingTheBytesInChunksGivesTheSameRecords) {
  const TempDir d;
  std::string csv(kMboHeader);
  for (int i = 0; i < 40; ++i) {
    csv += mboRow(kT0 + static_cast<std::uint64_t>(i), 'A', 'B', 100 + i, 1,
                  1000 + static_cast<std::uint64_t>(i),
                  static_cast<std::uint32_t>(10 + i));
  }
  const auto t = transcodeMboCsv(d.file("x.csv", csv), md("mbo"));
  ASSERT_TRUE(t.ok()) << t.status().ToString();

  const auto decode = [&](std::size_t chunk) {
    SymbolTable symbols;
    SymbolTable venues;
    DatabentoAdapter adapter(DatabentoAdapter::Config{}, symbols, venues);
    for (std::size_t off = 0; off < t->dbn.size(); off += chunk) {
      const std::size_t n = std::min(chunk, t->dbn.size() - off);
      EXPECT_TRUE(
          adapter.feed(std::span<const std::byte>(t->dbn.data() + off, n)).ok());
    }
    EXPECT_TRUE(adapter.finish().ok());
    std::vector<std::tuple<std::int64_t, std::uint64_t, std::int64_t>> out;
    for (const auto& b : adapter.bookEvents()) {
      out.emplace_back(b.header.tsEventNs, b.header.providerSeq, b.price);
    }
    return out;
  };
  const auto whole = decode(t->dbn.size());
  ASSERT_EQ(whole.size(), 40u);
  // 1, 7 and 101 all straddle the metadata block, the symbology section and the
  // 56-byte records, so no boundary in the file is also a feed boundary.
  EXPECT_EQ(decode(1), whole);
  EXPECT_EQ(decode(7), whole);
  EXPECT_EQ(decode(101), whole);
}

// ===========================================================================
// Retention (SPEC D6)
// ===========================================================================

TEST(DatabentoCsvRetention, WritesOnceThenReportsUnchanged) {
  const TempDir d;
  const std::vector<std::byte> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
  const auto path = d.at("f.dbn");

  const auto first = retain(path, bytes);
  ASSERT_TRUE(first.ok()) << first.status().ToString();
  EXPECT_FALSE(first->existed);
  EXPECT_TRUE(first->changed);

  const auto second = retain(path, bytes);
  ASSERT_TRUE(second.ok()) << second.status().ToString();
  EXPECT_TRUE(second->existed);
  EXPECT_FALSE(second->changed)
      << "identical bytes must report unchanged — that report IS the CSV -> DBN "
         "determinism check the ingest job prints";

  const std::vector<std::byte> other{std::byte{1}, std::byte{2}, std::byte{9}};
  const auto third = retain(path, other);
  ASSERT_TRUE(third.ok()) << third.status().ToString();
  EXPECT_TRUE(third->existed);
  EXPECT_TRUE(third->changed);

  const auto back = readFile(path);
  ASSERT_TRUE(back.ok()) << back.status().ToString();
  EXPECT_EQ(*back, other);
}

TEST(DatabentoCsvRetention, NoPartialFileSurvivesASuccessfulWrite) {
  const TempDir d;
  const std::vector<std::byte> bytes{std::byte{7}};
  const auto path = d.at("f.dbn");
  ASSERT_TRUE(retain(path, bytes).ok());
  // The write goes to `<path>.partial` and is renamed, so an interrupted write
  // never leaves a torn file that a later `ingest` reads as the retained raw.
  // D6 makes this file the thing L1 is rebuilt FROM; a torn one is worse than
  // an absent one.
  EXPECT_FALSE(std::filesystem::exists(path + ".partial"));
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(DatabentoCsvRetention, ReadingAnAbsentFileIsAnErrorNotAnEmptyBuffer) {
  const TempDir d;
  EXPECT_FALSE(readFile(d.at("nope.dbn")).ok());
}

// ===========================================================================
// MONOTONICITY — code-review finding #10, the one that sat under the
// completeness claim.
// ===========================================================================

TEST(DatabentoCsvTruncation, OutOfOrderTsEventIsRefusedRatherThanPublished) {
  const TempDir d;
  // `deliveredEnd()` publishes the LAST record's ts_event as the exclusive end
  // written to gma.ingest_windows, and that is only the delivered end if the
  // vendor emits in time order. The justification was an unchecked assumption
  // about the input; now it is asserted, and a violation is a REFUSAL rather
  // than a quietly smaller window.
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     mboRow(kT0, 'A', 'B', 100, 1, 9, 5) +
                                     mboRow(kT0 + 50, 'A', 'B', 101, 1, 10, 6) +
                                     mboRow(kT0 + 10, 'A', 'B', 102, 1, 11, 7));
  const auto r = transcodeMboCsv(p, md("mbo"));
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.status().message().find("ts_event went BACKWARDS"),
            std::string::npos)
      << r.status().message();
  // And it is a FINDING, not a could-not-run: the file read fine and what it
  // says is wrong.
  EXPECT_TRUE(isFinding(r.status()));
  EXPECT_EQ(exitCodeFor(r.status()), 1);

  // In order, the same three rows are accepted — so the refusal above is about
  // the ordering and not about the rows.
  const auto ok = d.file("ok.csv", std::string(kMboHeader) +
                                       mboRow(kT0, 'A', 'B', 100, 1, 9, 5) +
                                       mboRow(kT0 + 10, 'A', 'B', 102, 1, 11, 7) +
                                       mboRow(kT0 + 50, 'A', 'B', 101, 1, 10, 6));
  const auto good = transcodeMboCsv(ok, md("mbo"));
  ASSERT_TRUE(good.ok()) << good.status().ToString();
  EXPECT_EQ(good->lastTsEvent, kT0 + 50);
}

TEST(DatabentoCsvTruncation, RepeatedTimestampsAreNotADescent) {
  const TempDir d;
  // Equal consecutive ts_event is normal — one venue message group yields
  // several records — and must not trip the monotonicity check.
  const auto p = d.file("x.csv", std::string(kMboHeader) +
                                     mboRow(kT0, 'T', 'A', 100, 1, 0, 5) +
                                     mboRow(kT0, 'F', 'B', 100, 1, 9, 5) +
                                     mboRow(kT0, 'C', 'B', 100, 1, 9, 5));
  const auto r = transcodeMboCsv(p, md("mbo", 3));
  ASSERT_TRUE(r.ok()) << r.status().ToString();
  EXPECT_EQ(r->recordsAtLastTsEvent, 3u);
  EXPECT_TRUE(r->truncatedByLimit);
}

// ===========================================================================
// RETAIN ONLY IF DECODABLE — code-review finding #3, the worst of the review.
// ===========================================================================

TEST(DatabentoCsvRetention, AnExistingBaselineSurvivesUndecodableReplacement) {
  const TempDir d;
  const auto path = d.at("sample.dbn");

  // A good baseline, written the normal way.
  const auto csvPath = d.file(
      "x.csv", std::string(kMboHeader) + mboRow(kT0, 'A', 'B', 100, 10, 9, 5));
  const auto good = transcodeMboCsv(csvPath, md("mbo"));
  ASSERT_TRUE(good.ok()) << good.status().ToString();
  ASSERT_TRUE(ingest::retainOnlyIfDecodable(path, good->dbn).ok());
  {
    const auto back = readFile(path);
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(*back, good->dbn);
  }

  // Now try to replace it with bytes the adapter refuses. The encoder writes
  // what it is told, including a DBN version that does not exist; the DECODER
  // refuses an unknown version rather than guessing, which is correct — and is
  // exactly why the old order was fatal: it wrote first and discovered second.
  std::vector<std::byte> undecodable = good->dbn;
  ASSERT_GT(undecodable.size(), 4u);
  undecodable[3] = std::byte{7};  // the version byte

  const auto refused = ingest::retainOnlyIfDecodable(path, undecodable);
  ASSERT_FALSE(refused.ok());
  EXPECT_NE(refused.status().message().find("REFUSING to write"),
            std::string::npos)
      << refused.status().message();
  EXPECT_TRUE(isFinding(refused.status()));

  // THE ASSERTION THAT MATTERS: the D6 baseline is byte-for-byte intact. Before
  // the fix this file held the undecodable bytes and `gma_ingest reload` — whose
  // entire premise is "a reload must reload FROM something" — failed on it
  // permanently.
  const auto after = readFile(path);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(*after, good->dbn)
      << "the retained DBN was replaced by bytes the adapter refuses";

  // And the undecodable bytes really are undecodable, so the test is not
  // passing because the mutation was a no-op.
  EXPECT_NE(undecodable, good->dbn);
}

TEST(DatabentoCsvRetention, NoTempFileIsLeftBehindByARefusal) {
  const TempDir d;
  const auto path = d.at("sample.dbn");
  std::vector<std::byte> junk{std::byte{'D'}, std::byte{'B'}, std::byte{'N'},
                              std::byte{9}};
  EXPECT_FALSE(ingest::retainOnlyIfDecodable(path, junk).ok());
  EXPECT_FALSE(std::filesystem::exists(path));
  // The unique temp name (finding #9) is `<path>.partial.XXXXXX`, so check the
  // directory holds nothing matching rather than one fixed name.
  for (const auto& e : std::filesystem::directory_iterator(
           std::filesystem::path(path).parent_path())) {
    EXPECT_EQ(e.path().string().find(".partial"), std::string::npos)
        << "a temporary survived: " << e.path();
  }
}

// ===========================================================================
// THE EXIT-CODE CLASSIFICATION — code-review finding #1.
// ===========================================================================

TEST(DatabentoCsvVerdict, AnUntaggedStatusIsCouldNotRunAndATaggedOneIsAFinding) {
  const arrow::Status plain = arrow::Status::Invalid("something");
  EXPECT_FALSE(isFinding(plain));
  EXPECT_EQ(exitCodeFor(plain), 2) << "the default is COULD NOT RUN";

  const arrow::Status tagged = asFinding(arrow::Status::Invalid("something"));
  EXPECT_TRUE(isFinding(tagged));
  EXPECT_EQ(exitCodeFor(tagged), 1);
  EXPECT_NE(tagged.message().find("something"), std::string::npos)
      << "tagging must not eat the message";

  EXPECT_EQ(exitCodeFor(arrow::Status::OK()), 0);
  EXPECT_FALSE(isFinding(arrow::Status::OK()));
  // Tagging an OK status is a no-op rather than a lie.
  EXPECT_TRUE(asFinding(arrow::Status::OK()).ok());
  EXPECT_FALSE(isFinding(asFinding(arrow::Status::OK())));

  // An IOError tagged as a finding IS one: the classification is the site's
  // decision, not the status code's. "The retained DBN does not read back" was
  // naturally an IOError and is a finding.
  EXPECT_EQ(exitCodeFor(asFinding(arrow::Status::IOError("x"))), 1);
}

TEST(DatabentoCsvVerdict, TheFindingSitesInTheCsvReaderAreTagged) {
  const TempDir d;
  // Two tickers for one instrument_id: the data read fine and is wrong.
  auto second = mboRow(kT0 + 1, 'A', 'B', 100, 1, 10, 6);
  second.replace(second.rfind("ZZZZ"), 4, "YYYY");
  const auto twoTickers = transcodeMboCsv(
      d.file("a.csv", std::string(kMboHeader) +
                          mboRow(kT0, 'A', 'B', 100, 1, 9, 5) + second),
      md("mbo"));
  ASSERT_FALSE(twoTickers.ok());
  EXPECT_EQ(exitCodeFor(twoTickers.status()), 1);

  // A malformed header is a COULD-NOT-RUN: nothing about the data was judged,
  // the file simply is not the file this reader processes. The two must not
  // collapse onto one code — that is the whole point of finding #1.
  const auto badHeader = transcodeMboCsv(d.file("b.csv", "nope,nothing\n"), md("mbo"));
  ASSERT_FALSE(badHeader.ok());
  EXPECT_EQ(exitCodeFor(badHeader.status()), 2);
}

// ===========================================================================
// SQL QUOTING — code-review finding #5.
// ===========================================================================

TEST(DatabentoCsvSql, SqlQuoteEscapesWhatClickHousesGrammarNeeds) {
  EXPECT_EQ(ch::sqlQuote("MSFT"), "'MSFT'");
  EXPECT_EQ(ch::sqlQuote(""), "''");
  // The exploit from the review: `verify --symbol "x' OR 1=1 --"` made the
  // completeness read answer over every symbol in the table.
  EXPECT_EQ(ch::sqlQuote("x' OR 1=1 --"), "'x\\' OR 1=1 --'");
  EXPECT_EQ(ch::sqlQuote("back\\slash"), "'back\\\\slash'");
  // A value that is nothing but quote characters still comes out balanced.
  EXPECT_EQ(ch::sqlQuote("''"), "'\\'\\''");
}

// ===========================================================================
// ANTI-VACUITY. `data/CMakeLists.txt` globs `tests/*.cpp` WITHOUT
// CONFIGURE_DEPENDS (ENC-1495): a new file that was never configured in is not
// compiled and the binary reports "0 tests from 0 test suites" at EXIT 0. This
// catches the other half — cases removed from a file that IS compiled — and the
// total is checked by hand against the baseline on every gate run.
// ===========================================================================
TEST(DatabentoCsvAntiVacuity, EveryCaseInThisFileIsRegistered) {
  if (std::string(GTEST_FLAG_GET(filter)) != "*") {
    GTEST_SKIP() << "a filter is active; the count would be about the filter";
  }
  const auto* unit = ::testing::UnitTest::GetInstance();
  int found = 0;
  for (int i = 0; i < unit->total_test_suite_count(); ++i) {
    const auto* suite = unit->GetTestSuite(i);
    if (std::string(suite->name()).rfind("DatabentoCsv", 0) == 0) {
      found += suite->total_test_count();
    }
  }
  EXPECT_EQ(found, 38) << "the DatabentoCsv* suites in this file lost or gained a "
                          "case; update the number deliberately rather than "
                          "letting the count drift";
}

}  // namespace
}  // namespace gma::data::csv
