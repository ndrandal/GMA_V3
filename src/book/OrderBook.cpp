#include "gma/book/OrderBook.hpp"
#include <algorithm>
#include <cassert>
#include <sstream>

namespace gma {

// --------- Synthetic ID generator (D4) ---------
uint64_t OrderBook::nextSyntheticId(FeedScope s) {
    const uint64_t k = scopeKey(s);
    auto it = synthCounters_.find(k);
    if (it == synthCounters_.end()) {
        return synthCounters_.emplace(k, 1ULL).first->second++;
    }
    return (it->second)++;
}

// --------- Public API (wrappers) ---------
bool OrderBook::applyAdd(const Order& o) {
    std::scoped_lock lk(m_);
    Order copy = o; copy.feedId = 0; copy.epoch = 0;
    return addImpl(copy);
}

bool OrderBook::applyUpdate(uint64_t id,
                            std::optional<Price> newPrice,
                            std::optional<uint64_t> newSize) {
    std::scoped_lock lk(m_);
    return updateImpl(makeKey(id, FeedScope{0,0}, false), newPrice, newSize);
}

bool OrderBook::applyDelete(uint64_t id) {
    std::scoped_lock lk(m_);
    return deleteImpl(makeKey(id, FeedScope{0,0}, false));
}

bool OrderBook::applyAdd(const Order& o, FeedScope scope, bool idMissing) {
    std::scoped_lock lk(m_);
    Order copy = o;
    copy.feedId = scope.feedId;
    copy.epoch  = scope.epoch;
    if (idMissing || copy.id == 0) {
        copy.synthetic = true;
        copy.id = nextSyntheticId(scope);
    }
    return addImpl(copy);
}

bool OrderBook::applyUpdate(const OrderKey& key,
                            std::optional<Price> newPrice,
                            std::optional<uint64_t> newSize) {
    std::scoped_lock lk(m_);
    return updateImpl(key, newPrice, newSize);
}

bool OrderBook::applyDelete(const OrderKey& key) {
    std::scoped_lock lk(m_);
    return deleteImpl(key);
}

// --------- Trade ---------
bool OrderBook::applyTrade(Price tradePrice, uint64_t qty, Aggressor aggr) {
    if (qty == 0) return false;

    std::scoped_lock lk(m_);
    std::optional<Side> passive;

    if (aggr == Aggressor::Buy)      passive = Side::Ask;
    else if (aggr == Aggressor::Sell) passive = Side::Bid;
    else {
        if (!bids_.empty() && tradePrice.ticks <= bids_.begin()->first.ticks)      passive = Side::Bid;
        else if (!asks_.empty() && tradePrice.ticks >= asks_.begin()->first.ticks) passive = Side::Ask;
        else return false; // hidden/midpoint or empty book
    }

    uint64_t consumed = consumeAtLevel(*passive, tradePrice, qty);
    return consumed > 0;
}

// --------- Snapshots / Summaries ---------
void OrderBook::applySnapshotPerOrder(const std::vector<Order>& orders) {
    std::scoped_lock lk(m_);
    clearPerOrderUnlocked();
    for (const auto& o : orders) addImpl(o);
}

void OrderBook::applySnapshotAggregated(const std::vector<LevelSnapshotEntry>& levels) {
    std::scoped_lock lk(m_);
    clearAggregatedUnlocked();
    for (const auto& e : levels) {
        if (e.totalSize == 0) continue;
        LevelAgg& lvl = getOrCreateAggLevel(e.side, e.price);
        lvl.totalSize = e.totalSize;
        lvl.orderCount = e.orderCount.value_or(0);
    }
}

bool OrderBook::applyLevelSummary(Side side, Price price, uint64_t totalSize,
                                  std::optional<uint32_t> orderCount) {
    std::scoped_lock lk(m_);
    if (totalSize == 0) {
        if (side == Side::Bid) { auto it = bidsAgg_.find(price); if (it != bidsAgg_.end()) bidsAgg_.erase(it); }
        else                    { auto it = asksAgg_.find(price); if (it != asksAgg_.end()) asksAgg_.erase(it); }
        return true;
    }
    LevelAgg& lvl = getOrCreateAggLevel(side, price);
    bool changed = (lvl.totalSize != totalSize) || (orderCount && lvl.orderCount != *orderCount);
    lvl.totalSize = totalSize;
    if (orderCount) lvl.orderCount = *orderCount;
    return changed;
}

// --------- Priority updates ---------
bool OrderBook::applyPriority(uint64_t id, uint64_t newPriority) {
    std::scoped_lock lk(m_);
    return priorityImpl(makeKey(id, FeedScope{0,0}, false), newPriority);
}
bool OrderBook::applyPriority(const OrderKey& key, uint64_t newPriority) {
    std::scoped_lock lk(m_);
    return priorityImpl(key, newPriority);
}

// --------- TOB / queries ---------
std::optional<Price> OrderBook::bestBid() const {
    std::scoped_lock lk(m_);
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}
std::optional<Price> OrderBook::bestAsk() const {
    std::scoped_lock lk(m_);
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}
uint64_t OrderBook::bestBidSize() const {
    std::scoped_lock lk(m_);
    if (bids_.empty()) return 0ULL;
    return bids_.begin()->second.totalSize;
}
uint64_t OrderBook::bestAskSize() const {
    std::scoped_lock lk(m_);
    if (asks_.empty()) return 0ULL;
    return asks_.begin()->second.totalSize;
}
uint64_t OrderBook::levelSize(Side s, Price price) const {
    std::scoped_lock lk(m_);
    const PriceLevel* lvl = nullptr;
    if (s == Side::Bid) { auto it = bids_.find(price); if (it != bids_.end()) lvl = &it->second; }
    else                { auto it = asks_.find(price); if (it != asks_.end()) lvl = &it->second; }
    return lvl ? lvl->totalSize : 0ULL;
}

// Aggregated queries
std::optional<Price> OrderBook::bestBidAggregated() const {
    std::scoped_lock lk(m_);
    if (bidsAgg_.empty()) return std::nullopt;
    return bidsAgg_.begin()->first;
}
std::optional<Price> OrderBook::bestAskAggregated() const {
    std::scoped_lock lk(m_);
    if (asksAgg_.empty()) return std::nullopt;
    return asksAgg_.begin()->first;
}
uint64_t OrderBook::levelSizeAggregated(Side s, Price price) const {
    std::scoped_lock lk(m_);
    const LevelAgg* lvl = nullptr;
    if (s == Side::Bid) { auto it = bidsAgg_.find(price); if (it != bidsAgg_.end()) lvl = &it->second; }
    else                { auto it = asksAgg_.find(price); if (it != asksAgg_.end()) lvl = &it->second; }
    return lvl ? lvl->totalSize : 0ULL;
}

// ----- Depth helper -----
void OrderBook::forEachLevel(Side side, size_t n,
                             const std::function<void(Price, uint64_t)>& fn) const {
    std::vector<std::pair<Price, uint64_t>> out;
    {
        std::scoped_lock lk(m_);
        if (side == Side::Bid) {
            size_t i = 0;
            for (auto it = bids_.begin(); it != bids_.end() && i < n; ++it, ++i) {
                out.emplace_back(it->first, it->second.totalSize);
            }
        } else {
            size_t i = 0;
            for (auto it = asks_.begin(); it != asks_.end() && i < n; ++it, ++i) {
                out.emplace_back(it->first, it->second.totalSize);
            }
        }
    }
    for (const auto& p : out) fn(p.first, p.second);
}

// --------- Invariants ---------
bool OrderBook::checkInvariants(std::string* whyNot) const {
    std::scoped_lock lk(m_);
    auto checkSide = [&](const auto& ladder) {
        for (const auto& kv : ladder) {
            const auto& level = kv.second;
            uint64_t sum = 0;
            for (const auto& o : level.orders) sum += o.size;
            if (sum != level.totalSize) return false;
        }
        return true;
    };
    if (!checkSide(bids_) || !checkSide(asks_)) {
        if (whyNot) *whyNot = "per-order: level.totalSize mismatch with order sum";
        return false;
    }
    for (const auto& kv : byId_) {
        const auto& loc = kv.second;
        const PriceLevel* lvl = (loc.side == Side::Bid)
            ? (bids_.count(loc.price) ? &bids_.at(loc.price) : nullptr)
            : (asks_.count(loc.price) ? &asks_.at(loc.price) : nullptr);
        if (!lvl) { if (whyNot) *whyNot = "per-order: locator references missing level"; return false; }
        if (loc.it == std::list<Order>::iterator{}) { if (whyNot) *whyNot = "per-order: locator default iterator"; return false; }
        const Order& o = *loc.it;
        const OrderKey key = OrderKey{ o.id, o.feedId, o.epoch, o.synthetic };
        if (!(key == kv.first)) { if (whyNot) *whyNot = "per-order: locator iterator does not match key"; return false; }
    }
    auto checkAgg = [&](const auto& ladder) {
        for (const auto& kv : ladder) if (kv.second.totalSize == 0) return false;
        return true;
    };
    if (!checkAgg(bidsAgg_) || !checkAgg(asksAgg_)) {
        if (whyNot) *whyNot = "aggregated: zero-size level present";
        return false;
    }
    return true;
}

// --------- Private helpers (per-order) ---------
PriceLevel& OrderBook::getOrCreateLevel(Side s, Price price) {
    if (s == Side::Bid) { auto [it,_]=bids_.try_emplace(price); return it->second; }
    else                { auto [it,_]=asks_.try_emplace(price); return it->second; }
}
PriceLevel* OrderBook::findLevel(Side s, Price price) {
    if (s == Side::Bid) { auto it=bids_.find(price); return (it==bids_.end())?nullptr:&it->second; }
    else                { auto it=asks_.find(price); return (it==asks_.end())?nullptr:&it->second; }
}
void OrderBook::eraseLevelIfEmpty(Side s, Price price) {
    if (s == Side::Bid) { auto it=bids_.find(price); if (it!=bids_.end() && it->second.orders.empty()) bids_.erase(it); }
    else                { auto it=asks_.find(price); if (it!=asks_.end() && it->second.orders.empty()) asks_.erase(it); }
}

// --------- Private helpers (aggregated) ---------
LevelAgg& OrderBook::getOrCreateAggLevel(Side s, Price price) {
    if (s == Side::Bid) { auto [it,_]=bidsAgg_.try_emplace(price); return it->second; }
    else                { auto [it,_]=asksAgg_.try_emplace(price); return it->second; }
}
LevelAgg* OrderBook::findAggLevel(Side s, Price price) {
    if (s == Side::Bid) { auto it=bidsAgg_.find(price); return (it==bidsAgg_.end())?nullptr:&it->second; }
    else                { auto it=asksAgg_.find(price); return (it==asksAgg_.end())?nullptr:&it->second; }
}
void OrderBook::eraseAggLevelIfEmpty(Side s, Price price) {
    if (s == Side::Bid) { auto it=bidsAgg_.find(price); if (it!=bidsAgg_.end() && it->second.totalSize==0) bidsAgg_.erase(it); }
    else                { auto it=asksAgg_.find(price); if (it!=asksAgg_.end() && it->second.totalSize==0) asksAgg_.erase(it); }
}

// --------- Core impls ---------
uint64_t OrderBook::consumeAtLevel(Side passive, Price price, uint64_t qty) {
    PriceLevel* lvl = findLevel(passive, price);
    if (!lvl || qty == 0) return 0ULL;

    uint64_t remaining = qty;
    while (remaining > 0 && !lvl->orders.empty()) {
        auto itFront = lvl->orders.begin();
        Order& top = *itFront;
        const uint64_t take = (top.size <= remaining) ? top.size : remaining;
        top.size -= take;
        remaining -= take;
        lvl->totalSize -= take;

        if (top.size == 0) {
            const OrderKey k{ top.id, top.feedId, top.epoch, top.synthetic };
            auto byIt = byId_.find(k);
            if (byIt != byId_.end()) byId_.erase(byIt);
            lvl->orders.erase(itFront);
        } else {
            break;
        }
    }
    eraseLevelIfEmpty(passive, price);
    return qty - remaining;
}

bool OrderBook::addImpl(const Order& o) {
    const OrderKey key = makeKeyFromOrder(o);

    auto locIt = byId_.find(key);
    if (locIt != byId_.end()) {
        deleteImpl(key); // cancel+add semantics within same scope
    }

    PriceLevel& lvl = getOrCreateLevel(o.side, o.price);
    lvl.orders.push_back(o);
    auto itInLevel = std::prev(lvl.orders.end());
    lvl.totalSize += o.size;

    byId_.emplace(key, Locator{ o.side, o.price, itInLevel });
    return true;
}

bool OrderBook::updateImpl(const OrderKey& key,
                           std::optional<Price> newPrice,
                           std::optional<uint64_t> newSize) {
    auto it = byId_.find(key);
    if (it == byId_.end()) return false;

    Locator& loc = it->second;
    PriceLevel* lvl = findLevel(loc.side, loc.price);
    if (!lvl) { byId_.erase(it); return false; }

    Order& ord = *loc.it;
    const Price    oldPrice = ord.price;
    const uint64_t oldSize  = ord.size;

    const Price    tgtPrice = newPrice.has_value() ? *newPrice : oldPrice;
    const uint64_t tgtSize  = newSize.has_value() ? *newSize  : oldSize;

    if (tgtSize == 0) {
        if (lvl->totalSize >= oldSize) lvl->totalSize -= oldSize; else lvl->totalSize = 0;
        lvl->orders.erase(loc.it);
        eraseLevelIfEmpty(loc.side, oldPrice);
        byId_.erase(it);
        return true;
    }

    if (tgtPrice == oldPrice) {
        if (tgtSize == oldSize) return false;
        if (tgtSize > oldSize) lvl->totalSize += (tgtSize - oldSize);
        else                   lvl->totalSize -= (oldSize - tgtSize);
        ord.size = tgtSize;
        return true;
    }

    // migrate price level
    if (lvl->totalSize >= oldSize) lvl->totalSize -= oldSize; else lvl->totalSize = 0;
    lvl->orders.erase(loc.it);
    eraseLevelIfEmpty(loc.side, oldPrice);

    PriceLevel& newLvl = getOrCreateLevel(loc.side, tgtPrice);
    Order moved = ord;
    moved.price = tgtPrice;
    moved.size  = tgtSize;
    newLvl.orders.push_back(moved);
    auto newIt = std::prev(newLvl.orders.end());
    newLvl.totalSize += tgtSize;

    loc.price = tgtPrice;
    loc.it    = newIt;

    return true;
}

bool OrderBook::deleteImpl(const OrderKey& key) {
    auto it = byId_.find(key);
    if (it == byId_.end()) return false;

    const Locator loc = it->second;
    PriceLevel* lvl = findLevel(loc.side, loc.price);
    if (!lvl) { byId_.erase(it); return false; }

    const uint64_t sz = loc.it->size;
    if (lvl->totalSize >= sz) lvl->totalSize -= sz; else lvl->totalSize = 0;
    lvl->orders.erase(loc.it);
    eraseLevelIfEmpty(loc.side, loc.price);
    byId_.erase(it);
    return true;
}

// ----- Priority change: move-to-back -----
bool OrderBook::priorityImpl(const OrderKey& key, uint64_t newPriority) {
    auto it = byId_.find(key);
    if (it == byId_.end()) return false;

    Locator& loc = it->second;
    PriceLevel* lvl = findLevel(loc.side, loc.price);
    if (!lvl) { byId_.erase(it); return false; }

    Order& ord = *loc.it;
    if (ord.priority == newPriority) return false;
    ord.priority = newPriority;

    auto last = lvl->orders.end();
    --last;
    if (loc.it != last) {
        lvl->orders.splice(lvl->orders.end(), lvl->orders, loc.it);
        // iterator remains valid and still points to the same element
    }
    return true;
}

// --------- Clearers ---------
void OrderBook::clearPerOrderUnlocked() {
    bids_.clear(); asks_.clear(); byId_.clear();
}
void OrderBook::clearAggregatedUnlocked() {
    bidsAgg_.clear(); asksAgg_.clear();
}

// --------- D7/D8 helpers ---------
OrderKey OrderBook::applyAddGetKey(const Order& o, FeedScope scope, bool idMissing) {
    std::scoped_lock lk(m_);
    Order copy = o;
    copy.feedId = scope.feedId;
    copy.epoch  = scope.epoch;
    if (idMissing || copy.id == 0) {
        copy.synthetic = true;
        copy.id = nextSyntheticId(scope);
    }
    addImpl(copy);
    return makeKeyFromOrder(copy);
}

bool OrderBook::locate(const OrderKey& key, Side& outSide, Price& outPrice) const {
    std::scoped_lock lk(m_);
    auto it = byId_.find(key);
    if (it == byId_.end()) return false;
    outSide = it->second.side;
    outPrice = it->second.price;
    return true;
}

} // namespace gma
