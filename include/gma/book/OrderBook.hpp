#pragma once
#include <cstdint>
#include <map>
#include <list>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

namespace gma {

// --------- Sides / Trade aggressor ---------
enum class Side : uint8_t { Bid = 0, Ask = 1 };
enum class Aggressor : uint8_t { Unknown = 0, Buy = 1, Sell = 2 };

// --------- Price (integer ticks) ---------
struct Price {
    int64_t ticks = 0;
    friend bool operator<(const Price& a, const Price& b) noexcept { return a.ticks < b.ticks; }
    friend bool operator>(const Price& a, const Price& b) noexcept { return a.ticks > b.ticks; }
    friend bool operator==(const Price& a, const Price& b) noexcept { return a.ticks == b.ticks; }
    friend bool operator!=(const Price& a, const Price& b) noexcept { return a.ticks != b.ticks; }
};

// --------- Feed scoping ---------
struct FeedScope {
    uint32_t feedId = 0;  // e.g., connection/venue id
    uint32_t epoch  = 0;  // increments on session reset
};

struct OrderKey {
    uint64_t id = 0;
    uint32_t feedId = 0;
    uint32_t epoch  = 0;
    bool synthetic  = false;
};
struct OrderKeyHash {
    size_t operator()(const OrderKey& k) const noexcept {
        uint64_t a = k.id ^ (uint64_t(k.feedId) << 32) ^ uint64_t(k.epoch);
        a ^= (k.synthetic ? 0x9E3779B97F4A7C15ULL : 0ULL);
        a ^= a >> 33; a *= 0xff51afd7ed558ccdULL;
        a ^= a >> 33; a *= 0xc4ceb9fe1a85ec53ULL;
        a ^= a >> 33;
        return size_t(a);
    }
};
inline bool operator==(const OrderKey& a, const OrderKey& b) noexcept {
    return a.id==b.id && a.feedId==b.feedId && a.epoch==b.epoch && a.synthetic==b.synthetic;
}

// --------- Per-order structures ---------
struct Order {
    uint64_t id = 0;
    Side     side = Side::Bid;
    Price    price{};
    uint64_t size = 0;
    uint64_t priority = 0;  // recv seq or venue ts

    // D4 scope carried with the order
    uint32_t feedId = 0;
    uint32_t epoch  = 0;
    bool synthetic  = false;
};

struct PriceLevel {
    std::list<Order> orders;   // stable iterators
    uint64_t totalSize = 0;    // sum of order sizes in this level
};

// --------- Aggregated (level-only) structures ---------
struct LevelAgg {
    uint64_t totalSize = 0;
    uint32_t orderCount = 0;   // 0 if unknown
};

struct LevelSnapshotEntry {
    Side side;
    Price price;
    uint64_t totalSize;
    std::optional<uint32_t> orderCount;
};

// --------- OrderBook ---------
class OrderBook {
public:
    // Base API (tick-priced)
    bool applyAdd(const Order& o);
    bool applyUpdate(uint64_t id,
                     std::optional<Price> newPrice,
                     std::optional<uint64_t> newSize);
    bool applyDelete(uint64_t id);

    // Scoped variants
    bool applyAdd(const Order& o, FeedScope scope, bool idMissing);
    bool applyUpdate(const OrderKey& key,
                     std::optional<Price> newPrice,
                     std::optional<uint64_t> newSize);
    bool applyDelete(const OrderKey& key);

    // Trades
    bool applyTrade(Price tradePrice, uint64_t size, Aggressor aggr = Aggressor::Unknown);

    // Snapshots / summaries
    void applySnapshotPerOrder(const std::vector<Order>& orders);
    void applySnapshotAggregated(const std::vector<LevelSnapshotEntry>& levels);
    bool applyLevelSummary(Side side, Price price, uint64_t totalSize,
                           std::optional<uint32_t> orderCount = std::nullopt);

    // Priority updates (move-to-back on change)
    bool applyPriority(uint64_t id, uint64_t newPriority);
    bool applyPriority(const OrderKey& key, uint64_t newPriority);

    // TOB helpers (per-order)
    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    uint64_t bestBidSize() const;
    uint64_t bestAskSize() const;

    // Level size (per-order)
    uint64_t levelSize(Side s, Price price) const;

    // Aggregated queries
    std::optional<Price> bestBidAggregated() const;
    std::optional<Price> bestAskAggregated() const;
    uint64_t levelSizeAggregated(Side s, Price price) const;

    // Depth helper (per-order ladder): iterate top-N price levels
    void forEachLevel(Side side, size_t n,
                      const std::function<void(Price, uint64_t)>& fn) const;

    // Invariants
    bool checkInvariants(std::string* whyNot = nullptr) const;

    // D7: returning add overload (composite key after add)
    OrderKey applyAddGetKey(const Order& o, FeedScope scope, bool idMissing);

    // D8: locate existing order's (side,price)
    bool locate(const OrderKey& key, Side& outSide, Price& outPrice) const;

private:
    // Ladders keyed by Price (ticks)
    struct GreaterP { bool operator()(const Price& a, const Price& b) const noexcept { return a.ticks > b.ticks; } };
    struct LessP    { bool operator()(const Price& a, const Price& b) const noexcept { return a.ticks < b.ticks; } };
    using BidLadder = std::map<Price, PriceLevel, GreaterP>;
    using AskLadder = std::map<Price, PriceLevel, LessP>;
    BidLadder bids_;
    AskLadder asks_;

    using BidAgg = std::map<Price, LevelAgg, GreaterP>;
    using AskAgg = std::map<Price, LevelAgg, LessP>;
    BidAgg bidsAgg_;
    AskAgg asksAgg_;

    struct Locator {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<OrderKey, Locator, OrderKeyHash> byId_;

    mutable std::mutex m_;

private:
    // Per-order helpers (expect m_ held)
    PriceLevel& getOrCreateLevel(Side s, Price price);
    PriceLevel* findLevel(Side s, Price price);
    void eraseLevelIfEmpty(Side s, Price price);

    // Aggregated helpers (expect m_ held)
    LevelAgg& getOrCreateAggLevel(Side s, Price price);
    LevelAgg* findAggLevel(Side s, Price price);
    void eraseAggLevelIfEmpty(Side s, Price price);

    // Consume helper (expect m_ held)
    uint64_t consumeAtLevel(Side passive, Price price, uint64_t qty);

    // D4 helpers
    static inline uint64_t scopeKey(FeedScope s) noexcept { return (uint64_t(s.feedId) << 32) | uint64_t(s.epoch); }
    uint64_t nextSyntheticId(FeedScope s);
    OrderKey makeKeyFromOrder(const Order& o) const noexcept { return OrderKey{ o.id, o.feedId, o.epoch, o.synthetic }; }
    OrderKey makeKey(uint64_t id, FeedScope s, bool synthetic=false) const noexcept { return OrderKey{ id, s.feedId, s.epoch, synthetic }; }

    // Core impls (expect m_ held)
    bool addImpl(const Order& o);
    bool updateImpl(const OrderKey& key,
                    std::optional<Price> newPrice,
                    std::optional<uint64_t> newSize);
    bool deleteImpl(const OrderKey& key);
    bool priorityImpl(const OrderKey& key, uint64_t newPriority);

    // Clearers (expect m_ held)
    void clearPerOrderUnlocked();
    void clearAggregatedUnlocked();

    // Synthetic ID counters
    std::unordered_map<uint64_t, uint64_t> synthCounters_;
};

} // namespace gma
