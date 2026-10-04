// tests/feed/ItchAtomicsTest.cpp — ENC-1028
//
// `bid`, `ask`, `spread` and `timestamp` on the ITCH path, asserted against
// MESSAGES THE LIVE FEED ACTUALLY SENT (tests/feed/itch_live_capture.ndjson —
// 296 consecutive frames for one symbol, captured off
// wss://feed-sim.v3m.xyz/feed; see the .README beside it).
//
// Three things these tests exist to pin, in descending order of how quietly
// they would otherwise break:
//
//  1. THE TIME BASIS. ITCH stamps nanoseconds since UTC *midnight*;
//     `gma::Event::timestampNs` is nanoseconds since the *epoch*. The two
//     differ by ~1.7e18 ns, so a straight copy yields a timestamp that reads
//     as a believable time of day in 1970. The assertion below is deliberately
//     exact and date-independent: the stored atomic, reduced modulo one day,
//     must equal the raw number on the wire. A fabricated fixture could not
//     catch this class of error — the fabricator and the asserter would share
//     the mistake — which is why the fixture is a capture.
//
//  2. ABSENCE IS NOT ZERO. 0 is a perfectly plausible price. A symbol whose
//     book has no resting liquidity must leave `bid`/`ask`/`spread` UNSET in
//     the AtomicStore, not set to 0, and `AtomicStore::get` must answer
//     `std::nullopt`. Pinned on real `trade` frames with the book-building
//     frames withheld.
//
//  3. WHERE BID/ASK COME FROM. Not from a MarketFieldMap alias — no ITCH
//     message type carries a top-of-book field at all — but from the
//     reconstructed book, sampled at the instant the tick is dispatched. The
//     tap below re-derives the book's best from `OrderBookManager` inside
//     `compute()` and requires it to agree exactly, which is what makes this
//     an assertion about real book state rather than about a constant.
//
// NOTE ON `spread`: feed-simulator publishes `add_order` frames that cross the
// opposite side without a matching execution, so the reconstructed book is
// sometimes crossed and `ask - bid` is sometimes negative. That is in the
// source data (reproduced by an independent reconstruction outside GMA) and a
// crossed book being accepted unflagged is already pinned by
// tests/book/CrossedBookTest.cpp (ENC-807 L20). These tests therefore assert
// `spread == ask - bid` exactly and never `spread > 0`.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/MarketTA.hpp"
#include "gma/book/OrderBookManager.hpp"
#include "gma/feed/ItchAdapter.hpp"
#include "gma/market/MarketFieldMap.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Config.hpp"
#include "gma/ws/WsFeedClient.hpp"

#include <boost/asio/io_context.hpp>
#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace gma;

namespace {

constexpr std::uint64_t kNsPerDay = 86'400'000'000'000ULL;

// The fixture's symbol and stockLocate. Both appear verbatim in the capture.
constexpr const char* kSymbol = "BLITZ";

// ---------------------------------------------------------------------------
// Fixture loading. A missing fixture is a FAIL(), never a skip
// (GMA_V3/CLAUDE.md design-record D10): a skipped data-driven test is
// indistinguishable from a passing one in ctest's summary.
// ---------------------------------------------------------------------------
std::vector<std::string> loadCapture() {
  const std::string path =
      std::string(GMA_TEST_SOURCE_DIR) + "/tests/feed/itch_live_capture.ndjson";
  std::ifstream in(path);
  if (!in) {
    ADD_FAILURE() << "ENC-1028 fixture not found: " << path
                  << "\nThis test asserts against real captured ITCH frames and"
                     " has nothing to assert without them.";
    return {};
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

// Every `timestamp` value present in the capture, as it appears on the wire.
std::set<std::uint64_t> rawWireTimestamps(const std::vector<std::string>& lines) {
  std::set<std::uint64_t> out;
  for (const auto& l : lines) {
    rapidjson::Document d;
    d.Parse(l.c_str());
    if (d.HasParseError() || !d.IsObject()) continue;
    if (d.HasMember("timestamp") && d["timestamp"].IsUint64()) {
      out.insert(d["timestamp"].GetUint64());
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Tap: observes every tick the REAL path dispatches, and cross-checks the
// injected top-of-book against the book itself at that instant.
// ---------------------------------------------------------------------------
struct TickObs {
  std::string             symbol;
  std::uint64_t           timestampNs = 0;
  double                  lastPrice   = 0.0;
  std::optional<double>   bid;
  std::optional<double>   ask;
  std::optional<double>   bookBid;   // re-derived from OrderBookManager
  std::optional<double>   bookAsk;
};

class TickTap : public engine::IEventComputer {
public:
  explicit TickTap(const OrderBookManager* obm) : obm_(obm) {}

  std::string_view eventType() const override { return "tick"; }

  void compute(const Event& tick, engine::ComputeContext&) override {
    TickObs o;
    o.symbol      = tick.symbol;
    o.timestampNs = tick.timestampNs;
    if (tick.payload && tick.payload->IsObject()) {
      const auto& p = *tick.payload;
      if (p.HasMember("lastPrice") && p["lastPrice"].IsNumber())
        o.lastPrice = p["lastPrice"].GetDouble();
      if (p.HasMember("bid") && p["bid"].IsNumber()) o.bid = p["bid"].GetDouble();
      if (p.HasMember("ask") && p["ask"].IsNumber()) o.ask = p["ask"].GetDouble();
    }
    if (obm_) {
      o.bookBid = obm_->bestBid(tick.symbol);
      o.bookAsk = obm_->bestAsk(tick.symbol);
    }
    std::lock_guard<std::mutex> lk(mx_);
    obs_.push_back(std::move(o));
  }

  std::vector<TickObs> snapshot() const {
    std::lock_guard<std::mutex> lk(mx_);
    return obs_;
  }

private:
  const OrderBookManager* obm_;
  mutable std::mutex      mx_;
  std::vector<TickObs>    obs_;
};

// ---------------------------------------------------------------------------
// The harness. Builds the REAL ingress — a real WsFeedClient over a real
// ItchAdapter, a real OrderBookManager, a real Dispatcher and a real
// MarketTickComputer carrying the PRODUCTION default MarketFieldMap — and
// drives frames through `handleMessage`, the seam immediately below the
// socket. Only the socket is absent; the translation, the book mutation, the
// emission ORDER and the dispatch are the shipped code.
// ---------------------------------------------------------------------------
struct Harness {
  util::Config                   cfg;
  AtomicStore                    store;
  rt::ThreadPool                 pool{2};
  OrderBookManager               obm;
  boost::asio::io_context        ioc;
  std::unique_ptr<Dispatcher>    dispatcher;
  TickTap*                       tap = nullptr;
  std::shared_ptr<ws::WsFeedClient> client;

  Harness() {
    registerBuiltinFunctions();
    dispatcher = std::make_unique<Dispatcher>(&pool, &store, cfg);

    // The production field map, defaults only. Nothing is configured by hand
    // here — if `bidFields`/`askFields` had to be set for this to work, this
    // test would be asserting the test's configuration, not the shipped one.
    dispatcher->addComputer(std::make_unique<MarketTickComputer>(
        cfg, market::MarketFieldMap{}));

    auto tapOwned = std::make_unique<TickTap>(&obm);
    tap = tapOwned.get();
    dispatcher->addComputer(std::move(tapOwned));

    client = std::make_shared<ws::WsFeedClient>(
        ioc, dispatcher.get(), &obm, "ws://unused.invalid/feed",
        std::make_unique<feed::ItchAdapter>(),
        std::vector<std::string>{"*"});
  }

  void feed(const std::vector<std::string>& lines) {
    for (const auto& l : lines) client->handleMessage(l);
    // ENC-1340: a timer/pool task already posted is not yet executed. Drain
    // before sampling anything the pool might still be about to write.
    pool.drain();
  }
};

} // namespace

// ===========================================================================
// 1. The time basis. The stored `timestamp` atomic is epoch nanos, and it is
//    the SOURCE's time of day — not a re-stamp, and not the raw since-midnight
//    number copied across.
// ===========================================================================
TEST(ItchAtomicsTest, TimestampIsEpochNanosCarryingTheSourceTimeOfDay) {
  const auto lines = loadCapture();
  ASSERT_FALSE(lines.empty());
  const auto wire = rawWireTimestamps(lines);
  ASSERT_FALSE(wire.empty()) << "capture carries no `timestamp` field at all";

  // The fixture really is on the since-midnight basis: every raw value is
  // under one day, i.e. none of them is an epoch timestamp.
  for (std::uint64_t t : wire) {
    ASSERT_LT(t, kNsPerDay)
        << "fixture value " << t << " is not a nanos-since-midnight offset; "
           "the capture's basis assumption is wrong";
  }

  Harness h;
  h.feed(lines);

  const auto obs = h.tap->snapshot();
  ASSERT_FALSE(obs.empty()) << "no tick was dispatched from 296 real frames";

  std::size_t withTime = 0;
  std::set<std::uint64_t> distinct;
  for (const auto& o : obs) {
    if (o.timestampNs == 0) continue;
    ++withTime;
    distinct.insert(o.timestampNs);

    // (a) It is on the EPOCH basis, not the wire's. 1.7e18 ns is 2023-11-14;
    //     any since-midnight value is below 8.64e13, so this single bound
    //     separates the two bases by five orders of magnitude.
    EXPECT_GT(o.timestampNs, 1'700'000'000'000'000'000ULL)
        << "timestamp " << o.timestampNs << " is not epoch nanos — it looks "
           "like the raw since-midnight value copied across";

    // (b) It is the SOURCE's time of day, exactly. Reducing modulo one day
    //     strips the date GMA supplied and must leave a number the feed
    //     actually sent. Date-independent, so this holds whenever it runs.
    EXPECT_EQ(wire.count(o.timestampNs % kNsPerDay), 1u)
        << "timestamp " << o.timestampNs << " reduces to "
        << (o.timestampNs % kNsPerDay)
        << ", which is not any value present in the capture — the time was "
           "re-stamped rather than carried";
  }
  EXPECT_GT(withTime, 10u) << "almost no tick carried a source time";

  // (c) It VARIES. A constant would satisfy (a) and (b) and still be wrong.
  EXPECT_GT(distinct.size(), 1u)
      << "every tick carried the same timestamp — it is a constant, not data";

  // (d) The stored atomic agrees with the last tick, exactly, and is a STRING
  //     because epoch nanos exceed 2^53 and a double would round it.
  const auto stored = h.store.get(kSymbol, "timestamp");
  ASSERT_TRUE(stored.has_value()) << "`timestamp` atomic was never written";
  ASSERT_TRUE(std::holds_alternative<std::string>(*stored))
      << "`timestamp` must be stored as a string; a double loses ns precision";
  std::uint64_t lastWithTime = 0;
  for (const auto& o : obs) if (o.timestampNs) lastWithTime = o.timestampNs;
  EXPECT_EQ(std::get<std::string>(*stored), std::to_string(lastWithTime));
}

// ===========================================================================
// 2. The conversion refuses rather than guessing, and handles the day
//    boundary. Pure arithmetic — `nowEpochNs` is a parameter, so this test has
//    no clock dependency of its own.
// ===========================================================================
TEST(ItchAtomicsTest, TimestampConversionRefusesRatherThanInventingATime) {
  using Itch = feed::ItchAdapter;
  // 2026-10-04T00:00:00Z, plus 12:35:00 into the day.
  constexpr std::uint64_t kMidnight = 1'791'072'000'000'000'000ULL;
  constexpr std::uint64_t kNoonish  = 45'300'000'000'000ULL;   // 12:35:00
  const std::uint64_t now = kMidnight + kNoonish;

  // Normal case: the feed's time of day on the receiver's UTC day.
  EXPECT_EQ(Itch::itchTimestampToEpochNs(kNoonish - 1'000'000'000ULL, now),
            kMidnight + kNoonish - 1'000'000'000ULL);

  // Refusals, every one of which yields 0 == "not reported".
  EXPECT_EQ(Itch::itchTimestampToEpochNs(0, now), 0u)
      << "a zero field must read as unset, not as exactly midnight UTC";
  EXPECT_EQ(Itch::itchTimestampToEpochNs(kNsPerDay, now), 0u)
      << "a full day is not a since-midnight offset";
  EXPECT_EQ(Itch::itchTimestampToEpochNs(now, now), 0u)
      << "an epoch-nanos value handed in under this field must be refused, "
         "never rebased";
  EXPECT_EQ(Itch::itchTimestampToEpochNs(kNoonish, kNsPerDay - 1), 0u)
      << "a pre-1970 receiver clock is nonsense; refuse";

  // Day boundary: the receiver has rolled past 00:00 UTC while a frame stamped
  // at 23:59:59 was in flight. Attributing it to "today" would place it ~24h
  // in the future, so it belongs to yesterday.
  const std::uint64_t justAfterMidnight = kMidnight + 500'000'000ULL;
  const std::uint64_t lateYesterday     = kNsPerDay - 1'000'000'000ULL;
  EXPECT_EQ(Itch::itchTimestampToEpochNs(lateYesterday, justAfterMidnight),
            kMidnight - 1'000'000'000ULL);

  // ...but a stamp merely a little ahead of our clock stays on today: the
  // slack absorbs feed/receiver skew without flipping the date.
  const std::uint64_t slightlyAhead = kNoonish + 60'000'000'000ULL;  // +60s
  EXPECT_EQ(Itch::itchTimestampToEpochNs(slightlyAhead, now),
            kMidnight + slightlyAhead);
}

// ===========================================================================
// 3. bid/ask come from the reconstructed book and agree with it exactly, at the
//    instant the tick is dispatched.
// ===========================================================================
TEST(ItchAtomicsTest, BidAskAreTheReconstructedBooksBestAtTickTime) {
  const auto lines = loadCapture();
  ASSERT_FALSE(lines.empty());

  Harness h;
  h.feed(lines);

  const auto obs = h.tap->snapshot();
  ASSERT_FALSE(obs.empty());

  std::size_t twoSided = 0, distinctBids = 0;
  std::set<double> bids;
  for (const auto& o : obs) {
    // Presence tracks the book exactly — never a sentinel on either side.
    EXPECT_EQ(o.bid.has_value(), o.bookBid.has_value())
        << "injected bid presence disagrees with the book for " << o.symbol;
    EXPECT_EQ(o.ask.has_value(), o.bookAsk.has_value());
    if (o.bid) {
      EXPECT_DOUBLE_EQ(*o.bid, *o.bookBid);
      bids.insert(*o.bid);
    }
    if (o.ask) EXPECT_DOUBLE_EQ(*o.ask, *o.bookAsk);
    if (o.bid && o.ask) ++twoSided;
  }
  distinctBids = bids.size();

  EXPECT_GT(twoSided, 10u)
      << "the capture produced almost no two-sided tick; it cannot support "
         "this assertion";

  // Real, moving prices — not a constant and not zero. BLITZ trades around
  // $120 in this capture.
  EXPECT_GT(distinctBids, 3u)
      << "the injected bid never moved across " << obs.size()
      << " ticks — that is a constant, not book data";
  for (double b : bids) EXPECT_GT(b, 1.0) << "bid " << b << " is not a price";

  // The stored atomics agree with the last two-sided tick, and `spread` is
  // exactly ask - bid (which on this data is sometimes NEGATIVE — see the
  // header note; a crossed book is source-data, pinned by CrossedBookTest).
  const TickObs* lastTwoSided = nullptr;
  for (const auto& o : obs) if (o.bid && o.ask) lastTwoSided = &o;
  ASSERT_NE(lastTwoSided, nullptr);

  auto sBid = h.store.get(kSymbol, "bid");
  auto sAsk = h.store.get(kSymbol, "ask");
  auto sSpr = h.store.get(kSymbol, "spread");
  ASSERT_TRUE(sBid.has_value()) << "`bid` atomic was never written";
  ASSERT_TRUE(sAsk.has_value()) << "`ask` atomic was never written";
  ASSERT_TRUE(sSpr.has_value()) << "`spread` atomic was never written";
  EXPECT_DOUBLE_EQ(std::get<double>(*sBid), *lastTwoSided->bid);
  EXPECT_DOUBLE_EQ(std::get<double>(*sAsk), *lastTwoSided->ask);
  EXPECT_DOUBLE_EQ(std::get<double>(*sSpr),
                   std::get<double>(*sAsk) - std::get<double>(*sBid));
}

// ===========================================================================
// 4. No ITCH message carries a bid or an ask. This is the load-bearing fact
//    behind test 3's design: the field-alias layer could not have populated
//    these fields however it was configured, so it is not the mechanism.
// ===========================================================================
TEST(ItchAtomicsTest, NoItchMessageTypeCarriesABidOrAnAsk) {
  const auto lines = loadCapture();
  ASSERT_FALSE(lines.empty());

  std::set<std::string> types;
  for (const auto& l : lines) {
    rapidjson::Document d;
    d.Parse(l.c_str());
    ASSERT_FALSE(d.HasParseError()) << "unparseable captured frame: " << l;
    ASSERT_TRUE(d.IsObject());
    if (d.HasMember("type") && d["type"].IsString()) types.insert(d["type"].GetString());
    for (const char* k : {"bid", "ask", "bidPrice", "askPrice",
                          "bestBid", "bestAsk", "spread"}) {
      EXPECT_FALSE(d.HasMember(k))
          << "a captured ITCH frame carries `" << k
          << "` after all — the premise that bid/ask must come from the book "
             "needs revisiting: " << l;
    }
  }
  // Guard against a vacuous pass on a degenerate capture.
  EXPECT_GE(types.size(), 5u)
      << "the capture covers too few message types to support this claim; "
         "types seen: " << types.size();
}

// ===========================================================================
// 5. ABSENCE IS NOT ZERO. Real `trade` frames with every book-building frame
//    withheld: a tick is produced (so the path ran), the book stays empty, and
//    bid/ask/spread are UNSET rather than 0.
// ===========================================================================
TEST(ItchAtomicsTest, AbsentBookLeavesBidAskUnsetRatherThanZero) {
  const auto all = loadCapture();
  ASSERT_FALSE(all.empty());

  // Keep only `trade` frames. routeTrade needs no prior order state, so these
  // still produce ticks; nothing here can add liquidity to the book.
  std::vector<std::string> tradesOnly;
  for (const auto& l : all) {
    rapidjson::Document d;
    d.Parse(l.c_str());
    if (d.HasParseError() || !d.IsObject()) continue;
    if (d.HasMember("type") && d["type"].IsString() &&
        std::string(d["type"].GetString()) == "trade") {
      tradesOnly.push_back(l);
    }
  }
  ASSERT_GT(tradesOnly.size(), 10u);

  Harness h;
  h.feed(tradesOnly);

  // The path ran: a price and a volume are there.
  const auto lastPrice = h.store.get(kSymbol, "lastPrice");
  ASSERT_TRUE(lastPrice.has_value())
      << "no tick reached the store, so this test proves nothing about absence";
  EXPECT_GT(std::get<double>(*lastPrice), 1.0);

  // And the book never had a side, so these three are ABSENT. `has_value()`
  // false is the only representation of "not reported" — there is no sentinel
  // to confuse with a real price, and 0.0 would be a real price.
  for (const char* field : {"bid", "ask", "spread"}) {
    const auto v = h.store.get(kSymbol, field);
    EXPECT_FALSE(v.has_value())
        << "`" << field << "` was written with no book behind it: a value of "
        << (v && std::holds_alternative<double>(*v)
                ? std::get<double>(*v) : -1.0)
        << " stands in for 'not reported'";
  }
  EXPECT_FALSE(h.obm.bestBid(kSymbol).has_value());
  EXPECT_FALSE(h.obm.bestAsk(kSymbol).has_value());

  // A timestamp, by contrast, IS reported on a trade frame — so absence here
  // is per-field and not a blanket suppression.
  EXPECT_TRUE(h.store.get(kSymbol, "timestamp").has_value());
}

// ===========================================================================
// 6. THE `spread` COLLISION, pinned.
//
// `spread` is BOTH a market atomic (`ask - bid`, written by
// MarketTickComputer) and one of FunctionMap's 56 builtin reducers
// (`src/core/BuiltinFunctions.cpp`). `Dispatcher::computeAndStoreAtomics` runs
// AFTER the computers and `set()`s the builtin under the same flat bare key, so
// the moment ANY Listener exists on the symbol the bid-ask spread is replaced
// by a reducer over the subscribed field's history — and a Listener bound to
// `spread` is delivered that reducer, not the quote spread.
//
// Three facts about this test, because they decide how to read it:
//
//   * THE COLLISION IS NOT NEW. The `set(symbol, "spread", ask - bid)` line
//     predates ENC-1028; it was simply unreachable on ITCH because bid/ask were
//     never populated. Supplying them makes a pre-existing hazard live on this
//     path, which is exactly why it is pinned here rather than left as prose.
//     CLAUDE.md's ENC-1008 note names `spread`, `mean` and `median` as the
//     three colliding keys; this repo's builtin list and the market bare
//     vocabulary overlap in precisely those three and nothing else — `bid`,
//     `ask` and `timestamp` are clear.
//
//   * IT IS A PLAUSIBLE WRONG NUMBER, not a missing one, which is the bad
//     shape. The clobbered value is a real spread of something, just not of the
//     quote.
//
//   * THE MITIGATION EXISTS AND IS OFF BY DEFAULT.
//     `atomicKeyNamespaceByField = true` moves the builtin to
//     `<field>.spread`, leaving bare `spread` to the quote. The second half of
//     this test shows that, so the fix is recorded as tested rather than as a
//     suggestion.
// ===========================================================================
namespace {
// Minimal recording Listener-shaped node. The real `nodes::Listener` is not
// needed — `Dispatcher` only requires an `INode`, and using a bare one keeps
// this test out of the dispatch/subscription machinery ENC-1041 is editing.
class RecordingNode : public INode {
public:
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    if (std::holds_alternative<double>(sv.value))
      seen_.push_back(std::get<double>(sv.value));
  }
  void shutdown() noexcept override {}
  std::vector<double> seen() const {
    std::lock_guard<std::mutex> lk(mx_);
    return seen_;
  }
private:
  mutable std::mutex  mx_;
  std::vector<double> seen_;
};
} // namespace

TEST(ItchAtomicsTest, BareSpreadIsClobberedByTheFunctionMapBuiltin) {
  const auto lines = loadCapture();
  ASSERT_FALSE(lines.empty());

  // --- Default config: the builtin wins the bare `spread` key. ---
  {
    Harness h;
    auto node = std::make_shared<RecordingNode>();
    h.dispatcher->registerListener(kSymbol, "lastPrice", node);
    h.feed(lines);

    ASSERT_FALSE(node->seen().empty())
        << "the Listener never fired, so computeAndStoreAtomics never ran and "
           "this test cannot observe the collision";

    const auto bid = h.store.get(kSymbol, "bid");
    const auto ask = h.store.get(kSymbol, "ask");
    const auto spr = h.store.get(kSymbol, "spread");
    ASSERT_TRUE(bid.has_value());
    ASSERT_TRUE(ask.has_value());
    ASSERT_TRUE(spr.has_value());

    const double quote = std::get<double>(*ask) - std::get<double>(*bid);
    // THE DEFECT: bare `spread` is NOT the quote spread once a Listener exists.
    EXPECT_NE(std::get<double>(*spr), quote)
        << "bare `spread` happens to equal ask-bid here; if the collision has "
           "been fixed, invert this expectation and delete the mitigation half "
           "below";
    // It is the builtin over lastPrice history — a non-negative range.
    EXPECT_GE(std::get<double>(*spr), 0.0);
    // `bid` and `ask` are NOT builtin names, so they survive intact.
    EXPECT_GT(std::get<double>(*bid), 1.0);
    EXPECT_GT(std::get<double>(*ask), 1.0);
  }

  // --- Mitigation: atomicKeyNamespaceByField moves the builtin aside. ---
  {
    Harness h;
    h.cfg.atomicKeyNamespaceByField = true;
    // The Dispatcher copies cfg at construction, so rebuild it with the flag on.
    h.dispatcher = std::make_unique<Dispatcher>(&h.pool, &h.store, h.cfg);
    h.dispatcher->addComputer(std::make_unique<MarketTickComputer>(
        h.cfg, market::MarketFieldMap{}));
    auto tapOwned = std::make_unique<TickTap>(&h.obm);
    h.tap = tapOwned.get();
    h.dispatcher->addComputer(std::move(tapOwned));
    h.client = std::make_shared<ws::WsFeedClient>(
        h.ioc, h.dispatcher.get(), &h.obm, "ws://unused.invalid/feed",
        std::make_unique<feed::ItchAdapter>(),
        std::vector<std::string>{"*"});

    auto node = std::make_shared<RecordingNode>();
    h.dispatcher->registerListener(kSymbol, "lastPrice", node);
    h.feed(lines);
    ASSERT_FALSE(node->seen().empty());

    const auto bid = h.store.get(kSymbol, "bid");
    const auto ask = h.store.get(kSymbol, "ask");
    const auto spr = h.store.get(kSymbol, "spread");
    ASSERT_TRUE(bid.has_value());
    ASSERT_TRUE(ask.has_value());
    ASSERT_TRUE(spr.has_value());

    // With the flag on, bare `spread` keeps the quote spread...
    EXPECT_DOUBLE_EQ(std::get<double>(*spr),
                     std::get<double>(*ask) - std::get<double>(*bid));
    // ...and the builtin lands under its own namespaced key instead.
    const auto nsSpread = h.store.get(kSymbol, "lastPrice.spread");
    ASSERT_TRUE(nsSpread.has_value())
        << "the builtin did not move to `lastPrice.spread`; the mitigation "
           "this test documents does not actually work";
    EXPECT_GE(std::get<double>(*nsSpread), 0.0);
  }
}
