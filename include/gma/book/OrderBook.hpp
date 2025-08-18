#pragma once
#include <cstdint>
#include <map>
#include <list>
#include <optional>
#include <unordered_map>
#include <mutex>
#include <string>
#include <functional>
#include <vector>

namespace gma {

enum class Side : uint8_t { Bid = 0, Ask = 1 };

// D2
enum class Aggressor : uint8_t { Unknown = 0, Buy = 1, Sell = 2 };

struct FeedScope {
    uint32_t feedId = 0;  // e.g., venue/connection id
    uint32_t epoch  = 0;  // increments on session/reset
};

struct OrderKey {
    uint64_t id = 0;
    uint32_t feedId = 0;
    uint32_t epoch  = 0;
    bool synthetic  = false; // true if we generated an ID because the feed omitted it
    // equality + hash declared below
};
struct OrderKeyHash {
    size_t operator()(const OrderKey& k) const noexcept {
        // simple 128->64 fold; good enough for an unordered_map key
        uint64_t a = k.id ^ (uint64_t(k.feedId) << 32) ^ uint64_t(k.epoch);
        a ^= (k.synthetic ? 0x9E3779B97F4A7C15ULL : 0ULL);
        // final mix
        a ^= a >> 33; a *= 0xff51afd7ed558ccdULL;
        a ^= a >> 33; a *= 0xc4ceb9fe1a85ec53ULL;
        a ^= a >> 33;
        return size_t(a);
    }
};
inline bool operator==(const OrderKey& a, const OrderKey& b) noexcept {
    return a.id == b.id && a.feedId == b.feedId && a.epoch == b.epoch && a.synthetic == b.synthetic;
}


struct Order {
    uint64_t id = 0;
    Side side = Side::Bid;
    double price = 0.0;          // D5 will replace with ticks
    uint64_t size = 0;
    uint64_t priority = 0;       // recv seq or venue ts if available
    // NEW in D4: scope carried on each order so we can reference by composite key
    uint32_t feedId = 0;
    uint32_t epoch  = 0;
    bool synthetic  = false;
};

struct PriceLevel {
    std::list<Order> orders;     // stable iterators for locators
    uint64_t totalSize = 0;      // sum of order sizes in this level
};

// ---- D3: Aggregated level snapshot/summary structures ----
struct LevelAgg {
    uint64_t totalSize = 0;      // total visible size at this price
    uint32_t orderCount = 0;     // optional; 0 if unknown
};

struct LevelSnapshotEntry {
    Side side;
    double price;
    uint64_t totalSize;
    std::optional<uint32_t> orderCount; // may be null from some venues
};

class OrderBook {
public:
    // ----- D1 public API -----
    bool applyAdd(const Order& o);
    bool applyUpdate(uint64_t id,
                     std::optional<double> newPrice,
                     std::optional<uint64_t> newSize);
    bool applyDelete(uint64_t id);

    // ----- D2 public API -----
    bool applyTrade(double tradePrice, uint64_t size, Aggressor aggr = Aggressor::Unknown);

    // ----- D3 public API (snapshots & summaries) -----
    // Full per-order snapshot: clears current per-order state and rebuilds from 'orders'.
    void applySnapshotPerOrder(const std::vector<Order>& orders);

    // Full aggregated snapshot: clears current aggregated ladders and rebuilds.
    void applySnapshotAggregated(const std::vector<LevelSnapshotEntry>& levels);

    // Aggregated level summary/update (single level): sets size (and orderCount if provided).
    // If totalSize == 0, the level is erased.
    bool applyLevelSummary(Side side, double price, uint64_t totalSize,
                           std::optional<uint32_t> orderCount = std::nullopt);

    // ----- TOB helpers (per-order) -----
    std::optional<double> bestBid() const;
    std::optional<double> bestAsk() const;
    uint64_t bestBidSize() const;
    uint64_t bestAskSize() const;

    // Level size (per-order ladder)
    uint64_t levelSize(Side s, double price) const;

    // ----- D3: Aggregated query helpers (parallel structure) -----
    std::optional<double> bestBidAggregated() const;
    std::optional<double> bestAskAggregated() const;
    uint64_t levelSizeAggregated(Side s, double price) const;

    // Optional: debug/invariants
    bool checkInvariants(std::string* whyNot = nullptr) const;

    // Scoped adds: if idMissing==true, we’ll synthesize an ID unique to (feedId, epoch).
    bool applyAdd(const Order& o, FeedScope scope, bool idMissing);

    // Scoped update/delete use OrderKey explicitly.
    bool applyUpdate(const OrderKey& key,
                    std::optional<double> newPrice,
                    std::optional<uint64_t> newSize);
    bool applyDelete(const OrderKey& key);


private:
    // Per-order ladders
    using BidLadder = std::map<double, PriceLevel, std::greater<double>>;
    using AskLadder = std::map<double, PriceLevel, std::less<double>>;
    BidLadder bids_;
    AskLadder asks_;

    // Aggregated ladders (level-only)
    using BidAgg = std::map<double, LevelAgg, std::greater<double>>;
    using AskAgg = std::map<double, LevelAgg, std::less<double>>;
    BidAgg bidsAgg_;
    AskAgg asksAgg_;

    struct Locator {
        Side side;
        double price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<OrderKey, Locator, OrderKeyHash> byId_;

    mutable std::mutex m_; // per-book lock

private:
    // Per-order helpers (expect m_ held)
    PriceLevel& getOrCreateLevel(Side s, double price);
    PriceLevel* findLevel(Side s, double price);
    void eraseLevelIfEmpty(Side s, double price);

    // Aggregated helpers (expect m_ held)
    LevelAgg& getOrCreateAggLevel(Side s, double price);
    LevelAgg* findAggLevel(Side s, double price);
    void eraseAggLevelIfEmpty(Side s, double price); // if size==0

    // D2 consume helper (per-order, with m_ held)
    uint64_t consumeAtLevel(Side passive, double price, uint64_t qty);

    // Internal ops (expect m_ held)
    bool addImpl(const Order& o);
    bool updateImpl(uint64_t id, std::optional<double> newPrice, std::optional<uint64_t> newSize);
    bool deleteImpl(uint64_t id);

    OrderKey makeKeyFromOrder(const Order& o) const noexcept {
        return OrderKey{ o.id, o.feedId, o.epoch, o.synthetic };
    }
    OrderKey makeKey(uint64_t id, FeedScope s, bool synthetic=false) const noexcept {
        return OrderKey{ id, s.feedId, s.epoch, synthetic };
    }

    // synthetic id counters, keyed by (feedId, epoch) packed into 64 bits
    std::unordered_map<uint64_t, uint64_t> synthCounters_;
    static inline uint64_t scopeKey(FeedScope s) noexcept {
        return (uint64_t(s.feedId) << 32) | uint64_t(s.epoch);
    }


    uint64_t nextSyntheticId(FeedScope s); // generate per-scope synthetic ids

    // Replace declaration of delete/update impls to accept OrderKey
    bool updateImpl(const OrderKey& key,
                    std::optional<double> newPrice,
                    std::optional<uint64_t> newSize);
    bool deleteImpl(const OrderKey& key);


    // Clearers
    void clearPerOrderUnlocked();     // expect m_ held
    void clearAggregatedUnlocked();   // expect m_ held
};

} // namespace gma
