#pragma once
#include "gma/book/OrderBook.hpp"
#include "gma/book/DepthTypes.hpp"
#include "gma/Metrics.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <optional>
#include <list>
#include <functional>
#include <mutex>
#include <sstream>

namespace gma {

// Manager: sequencing, gap/stale control, tick quantization,
// event bus for deltas, venue-key resolver, validation & admin.

class OrderBookManager {
public:
    // ---- Tick-size configuration ----
    void   setTickSize(const std::string& symbol, double tickSize);
    double getTickSize(const std::string& symbol) const;
    Price  toTicks(const std::string& symbol, double px) const;
    double toDouble(const std::string& symbol, Price p) const;

    // ---- Feed state / epoch / sequencing ----
    struct FeedState {
        uint64_t lastSeq = 0;      // 0 means "unset"
        uint32_t epoch   = 0;
        bool     stale   = false;
    };
    bool onSeq(const std::string& symbol, uint64_t seq);      // returns true if you may apply the message
    void onReset(const std::string& symbol, uint32_t newEpoch);
    bool isStale(const std::string& symbol) const;
    FeedState getFeedState(const std::string& symbol) const;
    std::function<void(const std::string& symbol)> requestSnapshotFn; // invoked on gap/reset

    // ---- Venue-key resolver (for null-ID venues) ----
    void resolverSetCapacity(size_t cap); // global cap per symbol (default 4096)
    void resolverPut(const std::string& symbol, const std::string& venueKey, const OrderKey& key);
    std::optional<OrderKey> resolverGet(const std::string& symbol, const std::string& venueKey) const;

    // ---- Per-order mutations (double-priced entry points) ----
    OrderKey onAddGetKey(const std::string& symbol, uint64_t id, Side side, double price, uint64_t size,
                         uint64_t priority, FeedScope scope = {}, bool idMissing = false);
    bool onAdd(const std::string& symbol, uint64_t id, Side side, double price, uint64_t size,
               uint64_t priority, FeedScope scope = {}, bool idMissing = false);

    bool onUpdate(const std::string& symbol, uint64_t id, FeedScope scope,
                  std::optional<double> newPrice, std::optional<uint64_t> newSize,
                  bool synthetic = false);
    bool onUpdate(const std::string& symbol, const OrderKey& key,
                  std::optional<double> newPrice, std::optional<uint64_t> newSize);

    bool onDelete(const std::string& symbol, uint64_t id, FeedScope scope, bool synthetic = false);
    bool onDelete(const std::string& symbol, const OrderKey& key);

    bool onPriority(const std::string& symbol, uint64_t id, FeedScope scope, uint64_t newPriority, bool synthetic = false);
    bool onPriority(const std::string& symbol, const OrderKey& key, uint64_t newPriority);

    // Venue-key friendly overloads
    bool onAddWithVenueKey(const std::string& symbol, const std::string& venueKey,
                           uint64_t id, Side side, double price, uint64_t size,
                           uint64_t priority, FeedScope scope, bool idMissing);
    bool onUpdateByVenueKey(const std::string& symbol, const std::string& venueKey,
                            std::optional<double> newPrice, std::optional<uint64_t> newSize);
    bool onDeleteByVenueKey(const std::string& symbol, const std::string& venueKey);

    // Trades (double-priced entry point)
    bool onTrade(const std::string& symbol, double tradePrice, uint64_t size, Aggressor aggr = Aggressor::Unknown);

    // ---- Snapshots / summaries (accept optional snapshot seq to resume) ----
    void onSnapshotPerOrder(const std::string& symbol, const std::vector<Order>& tickOrders,
                            std::optional<uint64_t> snapshotSeq = std::nullopt);
    struct LevelSnapshotEntryD { Side side; double price; uint64_t totalSize; std::optional<uint32_t> orderCount; };
    void onSnapshotAggregated(const std::string& symbol, const std::vector<LevelSnapshotEntryD>& levelsD,
                              std::optional<uint64_t> snapshotSeq = std::nullopt);
    bool onLevelSummary(const std::string& symbol, Side side, double price, uint64_t totalSize,
                        std::optional<uint32_t> orderCount = std::nullopt);

    // ---- Queries ----
    std::optional<double> bestBid(const std::string& symbol) const;
    std::optional<double> bestAsk(const std::string& symbol) const;
    uint64_t bestBidSize(const std::string& symbol) const;
    uint64_t bestAskSize(const std::string& symbol) const;
    void depthN(const std::string& symbol, size_t n,
                std::vector<std::pair<double,uint64_t>>& bids,
                std::vector<std::pair<double,uint64_t>>& asks) const;

    // ---------- D8: Event bus + builders ----------
    using DeltaHandler = std::function<void(const BookDelta&)>;
    uint64_t subscribeDeltas(const std::string& symbol, DeltaHandler handler);
    void     unsubscribeDeltas(const std::string& symbol, uint64_t subId);
    DepthSnapshot buildSnapshot(const std::string& symbol, size_t levels) const;

    // ---- Admin / Observability (D10) ----
    MetricsSnapshot getStats() const;                              // snapshot all counters
    bool assertInvariants(const std::string& symbol, std::string* whyNot = nullptr) const;
    std::string dumpLadder(const std::string& symbol, size_t maxLevelsPerSide = 50) const;

private:
    // Books & ticks
    std::unordered_map<std::string, OrderBook> books_;
    mutable std::shared_mutex booksMx_;
    std::unordered_map<std::string, double> tickSize_;
    static constexpr double kDefaultTick = 1e-4;
    OrderBook& getOrCreateBook_(const std::string& symbol);

    // Feed state
    std::unordered_map<std::string, FeedState> feed_;
    bool gate_(const std::string& symbol) const;

    // Resolver (LRU per symbol)
    struct Lru {
        size_t cap = 4096;
        std::list<std::pair<std::string, OrderKey>> dq; // MRU at front
        std::unordered_map<std::string, std::list<std::pair<std::string, OrderKey>>::iterator> idx;
        void setCap(size_t c) { cap = c ? c : 1; trim(); }
        void put(const std::string& k, const OrderKey& v) {
            auto it = idx.find(k);
            if (it != idx.end()) { it->second->second = v; dq.splice(dq.begin(), dq, it->second); return; }
            dq.emplace_front(k, v); idx[k] = dq.begin(); trim();
        }
        std::optional<OrderKey> cget(const std::string& k) const {
            auto it = idx.find(k); if (it == idx.end()) return std::nullopt; return it->second->second;
        }
        void trim() { while (dq.size() > cap) { auto it = std::prev(dq.end()); idx.erase(it->first); dq.pop_back(); } }
    };
    mutable std::unordered_map<std::string, Lru> resolver_;

    // Event bus
    std::unordered_map<std::string, std::unordered_map<uint64_t, DeltaHandler>> subs_; // symbol -> (id->handler)
    std::mutex subsMx_;
    uint64_t nextSubId_ = 1;
    std::unordered_map<std::string, uint64_t> pubSeq_; // per-symbol publication seq

    // Metrics
    Metrics metrics_;

    // Helpers
    static int64_t quantizeTicks(double px, double tick);

    // Build & publish a delta if anything changed
    void maybePublishDelta_(const std::string& symbol,
                            const std::vector<LevelDelta>& levels,
                            std::optional<std::pair<Price,uint64_t>> newBid,
                            std::optional<std::pair<Price,uint64_t>> newAsk);

    // Wrap a mutation with before/after probes and auto-publish delta
    template <typename Fn>
    bool mutateWithDelta_(const std::string& symbol,
                          const std::vector<std::pair<Side,Price>>& candidates,
                          Fn&& mutator);

    // validation helpers
    inline bool validSide(Side s) const { return s == Side::Bid || s == Side::Ask; }
    bool validatePrice_(const std::string& symbol, double px) const;  // px > 0 and aligned to tick (within eps ok)
    static bool validateSize_(uint64_t sz) { return sz > 0; }
};

} // namespace gma
