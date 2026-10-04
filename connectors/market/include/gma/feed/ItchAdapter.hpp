#pragma once

#include "gma/feed/IFeedAdapter.hpp"
#include "gma/book/OrderBook.hpp"   // Side

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gma::feed {

/// Feed adapter for the NASDAQ ITCH JSON protocol.
///
/// Translates ITCH message types (stock_directory, add_order, order_executed,
/// order_cancel, order_delete, order_replace, trade, system_event) into
/// canonical FeedEvents.
///
/// Maintains internal state for:
///   - stockLocate → ticker mapping (ITCH pads to 8 chars)
///   - Live order tracking (needed to resolve partial fills / cancels)
class ItchAdapter : public IFeedAdapter {
public:
    std::vector<FeedEvent> translate(const std::string& rawMessage) override;

private:
    // ---- ITCH message handlers (each appends to `out`) ----
    void routeStockDirectory(const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeAddOrder      (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderExecuted (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderCancel   (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderDelete   (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeOrderReplace  (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeTrade         (const rapidjson::Value& doc, std::vector<FeedEvent>& out);
    void routeSystemEvent   (const rapidjson::Value& doc);

    // ---- Helpers ----
    static double parsePrice(const rapidjson::Value& v);

    /// Read the ITCH `timestamp` field, already converted to epoch nanos.
    /// Returns 0 when the message carries no usable timestamp (the canonical
    /// "not reported" marker — see Event::timestampNs).
    static uint64_t parseTimestampNs(const rapidjson::Value& doc);

    /// Build a TickEvent with lastPrice + volume fields for TA computation,
    /// carrying the source-reported event time on TickEvent::timestampNs.
    static TickEvent makeTradeTickEvent(const std::string& symbol,
                                        double price, uint64_t size,
                                        uint64_t timestampNs);

public:
    /// Convert an ITCH timestamp to NANOSECONDS SINCE THE UNIX EPOCH.
    ///
    /// ITCH 5.0 stamps every message with **nanoseconds since midnight UTC of
    /// the trading day**, and the wire carries no date whatsoever. GMA's
    /// canonical `Event::timestampNs` is epoch nanos. The two differ by ~1.7e18
    /// ns (54 years), so copying one into the other produces a timestamp that
    /// reads as a plausible *time of day* on 1970-01-01 — the exact class of
    /// silently-wrong number this platform rules worse than a refusal.
    ///
    /// So the date has to come from somewhere, and there is only one place it
    /// can come from: **the feed supplies the time of day, GMA supplies the UTC
    /// day.** That is a real limitation and it is the honest one — stated here
    /// rather than buried, because a consumer must know that the date is the
    /// receiver's and only the time-of-day is the source's.
    ///
    /// Rules, all of which return 0 (= not reported) rather than a guess:
    ///   * `nsSinceMidnightUtc == 0` is read as "field unset", not as exactly
    ///     00:00:00.000000000 UTC. A feed that genuinely means midnight to the
    ///     nanosecond is indistinguishable from one that left the field at its
    ///     zero value, and refusing is the safe half of that ambiguity.
    ///   * A value at or beyond one day is NOT a since-midnight offset. It is
    ///     refused outright rather than rebased, because rebasing it would
    ///     silently invent a time. (A feed handing us epoch nanos under this
    ///     field lands here.)
    ///   * A clock before 1970 + one day is nonsense; refuse.
    ///
    /// Day-boundary handling: the receiver can have rolled past 00:00 UTC while
    /// a message stamped just before it was in flight. A candidate more than
    /// `kFutureSlackNs` ahead of our own clock cannot belong to today, so it is
    /// attributed to the previous UTC day. The slack also absorbs modest clock
    /// skew between feed and receiver without flipping the date.
    static uint64_t itchTimestampToEpochNs(uint64_t nsSinceMidnightUtc,
                                           uint64_t nowEpochNs);

    static constexpr uint64_t kNsPerDay      = 86'400'000'000'000ULL;
    /// How far ahead of our own clock a stamp may be before it is read as
    /// yesterday's. One hour: far larger than any plausible feed/receiver skew
    /// or network delay, far smaller than a day.
    static constexpr uint64_t kFutureSlackNs = 3'600'000'000'000ULL;

private:

    // ---- ITCH protocol state ----

    /// stockLocate → ticker
    std::unordered_map<int, std::string> locateToSymbol_;

    /// Live order tracking for partial fill / cancel resolution.
    struct OrderState {
        std::string symbol;
        uint64_t    remainingShares;
        Side        side;
        double      price;
    };

    /// Hard cap on tracked orders (M4 / ENC-797). Bounds memory against a feed
    /// that adds orders without ever deleting them; when exceeded the oldest
    /// map bucket entry is evicted on the next add. ~1M entries keeps the map
    /// well under ~100 MB while comfortably covering any sane live session.
    static constexpr std::size_t kMaxTrackedOrders = 1'000'000;
    std::unordered_map<uint64_t, OrderState> orders_;
};

} // namespace gma::feed
