#include "gma/book/OrderBookManager.hpp"
#include <cmath>
#include <limits>
#include <unordered_set>
#include <iomanip>

using std::string;

namespace gma {

// ---------- Tick-size ----------
void OrderBookManager::setTickSize(const std::string& symbol, double tickSize) {
    std::unique_lock lk(booksMx_);
    tickSize_[symbol] = tickSize > 0 ? tickSize : kDefaultTick;
}
double OrderBookManager::getTickSize(const std::string& symbol) const {
    std::shared_lock lk(booksMx_);
    auto it = tickSize_.find(symbol);
    return (it == tickSize_.end()) ? kDefaultTick : it->second;
}
int64_t OrderBookManager::quantizeTicks(double px, double tick) {
    double q = px / tick; return std::llround(q + 1e-12);
}
Price OrderBookManager::toTicks(const std::string& symbol, double px) const {
    const double t = getTickSize(symbol);
    return Price{ quantizeTicks(px, t) };
}
double OrderBookManager::toDouble(const std::string& symbol, Price p) const {
    const double t = getTickSize(symbol);
    return static_cast<double>(p.ticks) * t;
}

// ---------- Book access ----------
OrderBook& OrderBookManager::getOrCreateBook_(const std::string& symbol) {
    {
        std::shared_lock rlk(booksMx_);
        auto it = books_.find(symbol);
        if (it != books_.end()) return it->second;
    }
    std::unique_lock wlk(booksMx_);
    return books_.try_emplace(symbol).first->second;
}
OrderBook& OrderBookManager::book(const std::string& symbol) { return getOrCreateBook_(symbol); }
const OrderBook* OrderBookManager::findBook_(const std::string& symbol) const {
    std::shared_lock rlk(booksMx_);
    auto it = books_.find(symbol);
    return (it != books_.end()) ? &it->second : nullptr;
}

// ---------- validation helpers ----------
bool OrderBookManager::validatePrice_(const std::string& symbol, double px) const {
    if (!(px > 0.0)) return false;
    const double t = getTickSize(symbol);
    double q = px / t;
    double r = std::fabs(q - std::round(q));
    return r < 1e-8;
}

// ---------- Feed state / Sequencing ----------
bool OrderBookManager::onSeq(const std::string& symbol, uint64_t seq) {
    std::lock_guard<std::mutex> lk(feedMx_);
    auto& st = feed_[symbol];
    if (st.stale) { metrics_.incDroppedStale(); return false; }
    if (st.lastSeq == 0) { st.lastSeq = seq; return true; }
    if (seq == st.lastSeq + 1) { st.lastSeq = seq; return true; }
    // GAP
    st.stale = true;
    metrics_.incSeqGap();
    metrics_.incStaleTransition();
    if (requestSnapshotFn) requestSnapshotFn(symbol);
    return false;
}

void OrderBookManager::onReset(const std::string& symbol, uint32_t newEpoch) {
    std::lock_guard<std::mutex> lk(feedMx_);
    auto& st = feed_[symbol];
    st.epoch = newEpoch; st.stale = true; st.lastSeq = 0;
    metrics_.incSeqReset();
    metrics_.incStaleTransition();
    if (requestSnapshotFn) requestSnapshotFn(symbol);
}

bool OrderBookManager::isStale(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(feedMx_);
    auto it = feed_.find(symbol); if (it == feed_.end()) return false;
    return it->second.stale;
}
OrderBookManager::FeedState OrderBookManager::getFeedState(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(feedMx_);
    auto it = feed_.find(symbol); if (it == feed_.end()) return FeedState{};
    return it->second;
}
bool OrderBookManager::gate_(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(feedMx_);
    auto it = feed_.find(symbol); if (it == feed_.end()) return false;
    return it->second.stale;
}

// ---------- Resolver ----------
void OrderBookManager::resolverSetCapacity(size_t cap) {
    std::lock_guard<std::mutex> lk(resolverMx_);
    for (auto& kv : resolver_) kv.second.setCap(cap);
}
void OrderBookManager::resolverPut(const std::string& symbol, const std::string& venueKey, const OrderKey& key) {
    std::lock_guard<std::mutex> lk(resolverMx_);
    resolver_[symbol].put(venueKey, key);
}
std::optional<OrderKey> OrderBookManager::resolverGet(const std::string& symbol, const std::string& venueKey) const {
    std::lock_guard<std::mutex> lk(resolverMx_);
    auto it = resolver_.find(symbol); if (it == resolver_.end()) return std::nullopt;
    return it->second.cget(venueKey);
}

// ---------- Event bus ----------
uint64_t OrderBookManager::subscribeDeltas(const std::string& symbol, DeltaHandler handler) {
    std::lock_guard<std::mutex> lk(subsMx_);
    uint64_t id = nextSubId_++;
    subs_[symbol][id] = std::move(handler);
    return id;
}
void OrderBookManager::unsubscribeDeltas(const std::string& symbol, uint64_t subId) {
    std::lock_guard<std::mutex> lk(subsMx_);
    auto it = subs_.find(symbol);
    if (it == subs_.end()) return;
    it->second.erase(subId);
}

void OrderBookManager::maybePublishDelta_(const std::string& symbol,
                                          const std::vector<LevelDelta>& levels,
                                          std::optional<std::pair<Price,uint64_t>> newBid,
                                          std::optional<std::pair<Price,uint64_t>> newAsk) {
    if (levels.empty() && !newBid && !newAsk) return;

    BookDelta d;
    d.symbol = symbol;
    d.levels = levels;
    d.bid = newBid;
    d.ask = newAsk;

    // metrics
    metrics_.incDeltasPublished();

    // fan-out (pubSeq_ and subs_ both under subsMx_)
    std::unordered_map<uint64_t, DeltaHandler> handlersCopy;
    {
        std::lock_guard<std::mutex> lk(subsMx_);
        d.seq = ++pubSeq_[symbol];
        auto it = subs_.find(symbol);
        if (it != subs_.end()) handlersCopy = it->second;
    }
    for (auto& kv : handlersCopy) kv.second(d);
}

// before/after wrapper that computes level & TOB changes
template <typename Fn>
bool OrderBookManager::mutateWithDelta_(const std::string& symbol,
                                        const std::vector<std::pair<Side,Price>>& candidates,
                                        Fn&& mutator) {
    OrderBook& b = book(symbol);

    // Pre-TOB
    auto preBid = b.bestBid();
    uint64_t preBidSz = b.bestBidSize();
    auto preAsk = b.bestAsk();
    uint64_t preAskSz = b.bestAskSize();

    // Probes
    struct Probe { Side s; Price p; uint64_t before; };
    std::vector<Probe> probes; probes.reserve(candidates.size());
    auto addProbe = [&](Side s, Price p){
        for (const auto& pr : probes) if (pr.s==s && pr.p.ticks==p.ticks) return;
        probes.push_back(Probe{s,p,b.levelSize(s,p)});
    };
    for (auto& c : candidates) addProbe(c.first, c.second);

    // Mutate
    bool changed = mutator();
    if (!changed) return false;

    // Post-TOB
    auto postBid = b.bestBid();
    uint64_t postBidSz = b.bestBidSize();
    auto postAsk = b.bestAsk();
    uint64_t postAskSz = b.bestAskSize();

    // Level deltas
    std::vector<LevelDelta> levels;
    levels.reserve(probes.size());
    for (auto& pr : probes) {
        uint64_t after = b.levelSize(pr.s, pr.p);
        if (after != pr.before) levels.push_back(LevelDelta{ pr.s, pr.p, after });
    }

    // TOB deltas
    std::optional<std::pair<Price,uint64_t>> nbid, nask;
    const bool bidChanged = (preBid.has_value() != postBid.has_value()) ||
                            (preBid && postBid && (preBid->ticks != postBid->ticks || preBidSz != postBidSz));
    const bool askChanged = (preAsk.has_value() != postAsk.has_value()) ||
                            (preAsk && postAsk && (preAsk->ticks != postAsk->ticks || preAskSz != postAskSz));
    if (postBid && bidChanged) nbid = std::make_pair(*postBid, postBidSz);
    if (postAsk && askChanged) nask = std::make_pair(*postAsk, postAskSz);

    maybePublishDelta_(symbol, levels, nbid, nask);
    return true;
}

// ---------- Adds ----------
OrderKey OrderBookManager::onAddGetKey(const std::string& symbol, uint64_t id, Side side, double price, uint64_t size,
                                       uint64_t priority, FeedScope scope, bool idMissing) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return OrderKey{0,scope.feedId,scope.epoch,true}; }
    if (!validSide(side) || !validatePrice_(symbol, price) || !validateSize_(size)) { metrics_.incDroppedMalformed(); return {}; }
    metrics_.incAdds();

    Order o; o.id=id; o.side=side; o.price=toTicks(symbol,price); o.size=size; o.priority=priority;
    auto candidates = std::vector<std::pair<Side,Price>>{ {side, o.price} };
    OrderKey outKey{};
    mutateWithDelta_(symbol, candidates, [&]{
        outKey = book(symbol).applyAddGetKey(o, scope, idMissing);
        return true;
    });
    return outKey;
}

bool OrderBookManager::onAdd(const std::string& symbol, uint64_t id, Side side, double price, uint64_t size,
                             uint64_t priority, FeedScope scope, bool idMissing) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!validSide(side) || !validatePrice_(symbol, price) || !validateSize_(size)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incAdds();

    Order o; o.id=id; o.side=side; o.price=toTicks(symbol,price); o.size=size; o.priority=priority;
    auto candidates = std::vector<std::pair<Side,Price>>{ {side, o.price} };
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyAdd(o, scope, idMissing); });
}

// ---------- Update/Delete/Priority ----------
bool OrderBookManager::onUpdate(const std::string& symbol, uint64_t id, FeedScope scope,
                                std::optional<double> newPrice, std::optional<uint64_t> newSize,
                                bool synthetic) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!newPrice && !newSize) { metrics_.incDroppedMalformed(); return false; }
    if (newPrice && !validatePrice_(symbol, *newPrice)) { metrics_.incDroppedMalformed(); return false; }
    if (newSize && !validateSize_(*newSize)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incUpdates();

    std::optional<Price> p; if (newPrice) p = toTicks(symbol, *newPrice);
    OrderKey key{ id, scope.feedId, scope.epoch, synthetic };

    Side oldSide; Price oldPrice;
    std::vector<std::pair<Side,Price>> candidates;
    if (book(symbol).locate(key, oldSide, oldPrice)) candidates.emplace_back(oldSide, oldPrice);
    if (newPrice) candidates.emplace_back(oldSide, toTicks(symbol, *newPrice));
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyUpdate(key, p, newSize); });
}

bool OrderBookManager::onUpdate(const std::string& symbol, const OrderKey& key,
                                std::optional<double> newPrice, std::optional<uint64_t> newSize) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!newPrice && !newSize) { metrics_.incDroppedMalformed(); return false; }
    if (newPrice && !validatePrice_(symbol, *newPrice)) { metrics_.incDroppedMalformed(); return false; }
    if (newSize && !validateSize_(*newSize)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incUpdates();

    Side oldSide; Price oldPrice;
    std::vector<std::pair<Side,Price>> candidates;
    if (book(symbol).locate(key, oldSide, oldPrice)) candidates.emplace_back(oldSide, oldPrice);
    if (newPrice) candidates.emplace_back(oldSide, toTicks(symbol, *newPrice));

    std::optional<Price> p; if (newPrice) p = toTicks(symbol, *newPrice);
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyUpdate(key, p, newSize); });
}

bool OrderBookManager::onDelete(const std::string& symbol, uint64_t id, FeedScope scope, bool synthetic) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    metrics_.incDeletes();
    OrderKey key{ id, scope.feedId, scope.epoch, synthetic };

    Side s; Price p; std::vector<std::pair<Side,Price>> candidates;
    if (book(symbol).locate(key, s, p)) candidates.emplace_back(s, p);
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyDelete(key); });
}
bool OrderBookManager::onDelete(const std::string& symbol, const OrderKey& key) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    metrics_.incDeletes();
    Side s; Price p; std::vector<std::pair<Side,Price>> candidates;
    if (book(symbol).locate(key, s, p)) candidates.emplace_back(s, p);
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyDelete(key); });
}

bool OrderBookManager::onPriority(const std::string& symbol, uint64_t id, FeedScope scope, uint64_t newPriority, bool synthetic) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    metrics_.incPriorities();
    OrderKey key{ id, scope.feedId, scope.epoch, synthetic };
    return mutateWithDelta_(symbol, {}, [&]{ return book(symbol).applyPriority(key, newPriority); });
}
bool OrderBookManager::onPriority(const std::string& symbol, const OrderKey& key, uint64_t newPriority) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    metrics_.incPriorities();
    return mutateWithDelta_(symbol, {}, [&]{ return book(symbol).applyPriority(key, newPriority); });
}

// ---------- Venue-key wrappers ----------
bool OrderBookManager::onAddWithVenueKey(const std::string& symbol, const std::string& venueKey,
                                         uint64_t id, Side side, double price, uint64_t size,
                                         uint64_t priority, FeedScope scope, bool idMissing) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!validSide(side) || !validatePrice_(symbol, price) || !validateSize_(size)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incAdds();
    auto k = onAddGetKey(symbol, id, side, price, size, priority, scope, idMissing);
    resolverPut(symbol, venueKey, k);
    return true;
}
bool OrderBookManager::onUpdateByVenueKey(const std::string& symbol, const std::string& venueKey,
                                          std::optional<double> newPrice, std::optional<uint64_t> newSize) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    auto k = resolverGet(symbol, venueKey); if (!k) { metrics_.incDroppedMalformed(); return false; }
    return onUpdate(symbol, *k, newPrice, newSize);
}
bool OrderBookManager::onDeleteByVenueKey(const std::string& symbol, const std::string& venueKey) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    auto k = resolverGet(symbol, venueKey); if (!k) { metrics_.incDroppedMalformed(); return false; }
    return onDelete(symbol, *k);
}

// ---------- Trades ----------
bool OrderBookManager::onTrade(const std::string& symbol, double tradePrice, uint64_t size, Aggressor aggr) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!validatePrice_(symbol, tradePrice) || !validateSize_(size)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incTrades();

    Price tp = toTicks(symbol, tradePrice);
    OrderBook& b = book(symbol);
    std::vector<std::pair<Side,Price>> candidates;
    if (b.levelSize(Side::Bid, tp) > 0) candidates.emplace_back(Side::Bid, tp);
    if (b.levelSize(Side::Ask, tp) > 0) candidates.emplace_back(Side::Ask, tp);
    return mutateWithDelta_(symbol, candidates, [&]{ return b.applyTrade(tp, size, aggr); });
}

// ---------- Snapshots / summaries ----------
void OrderBookManager::onSnapshotPerOrder(const std::string& symbol, const std::vector<Order>& tickOrders,
                                          std::optional<uint64_t> snapshotSeq) {
    book(symbol).applySnapshotPerOrder(tickOrders);
    {
        std::lock_guard<std::mutex> lk(feedMx_);
        auto& st = feed_[symbol];
        st.stale = false;
        if (snapshotSeq) st.lastSeq = *snapshotSeq;
    }
    metrics_.incSnapshots();

    auto bb = book(symbol).bestBid(); uint64_t bbs = book(symbol).bestBidSize();
    auto ba = book(symbol).bestAsk(); uint64_t bas = book(symbol).bestAskSize();
    std::optional<std::pair<Price,uint64_t>> nbid = bb ? std::make_pair(*bb,bbs) : std::optional<std::pair<Price,uint64_t>>{};
    std::optional<std::pair<Price,uint64_t>> nask = ba ? std::make_pair(*ba,bas) : std::optional<std::pair<Price,uint64_t>>{};
    maybePublishDelta_(symbol, {}, nbid, nask);
}
void OrderBookManager::onSnapshotAggregated(const std::string& symbol, const std::vector<LevelSnapshotEntryD>& levelsD,
                                            std::optional<uint64_t> snapshotSeq) {
    std::vector<LevelSnapshotEntry> levels; levels.reserve(levelsD.size());
    for (const auto& e : levelsD) levels.push_back(LevelSnapshotEntry{ e.side, toTicks(symbol, e.price), e.totalSize, e.orderCount });
    book(symbol).applySnapshotAggregated(levels);
    {
        std::lock_guard<std::mutex> lk(feedMx_);
        auto& st = feed_[symbol]; st.stale = false; if (snapshotSeq) st.lastSeq = *snapshotSeq;
    }
    metrics_.incSnapshots();

    auto bb = book(symbol).bestBid(); uint64_t bbs = book(symbol).bestBidSize();
    auto ba = book(symbol).bestAsk(); uint64_t bas = book(symbol).bestAskSize();
    std::optional<std::pair<Price,uint64_t>> nbid = bb ? std::make_pair(*bb,bbs) : std::optional<std::pair<Price,uint64_t>>{};
    std::optional<std::pair<Price,uint64_t>> nask = ba ? std::make_pair(*ba,bas) : std::optional<std::pair<Price,uint64_t>>{};
    maybePublishDelta_(symbol, {}, nbid, nask);
}
bool OrderBookManager::onLevelSummary(const std::string& symbol, Side side, double price, uint64_t totalSize,
                                      std::optional<uint32_t> orderCount) {
    if (gate_(symbol)) { metrics_.incDroppedStale(); return false; }
    if (!validSide(side) || !validatePrice_(symbol, price)) { metrics_.incDroppedMalformed(); return false; }
    metrics_.incSummaries();

    Price p = toTicks(symbol, price);
    auto candidates = std::vector<std::pair<Side,Price>>{ {side,p} };
    return mutateWithDelta_(symbol, candidates, [&]{ return book(symbol).applyLevelSummary(side, p, totalSize, orderCount); });
}

// ---------- Queries ----------
std::optional<double> OrderBookManager::bestBid(const std::string& symbol) const {
    const OrderBook* b = findBook_(symbol);
    if (!b) return std::nullopt;
    auto p = b->bestBid(); if (!p) return std::nullopt; return toDouble(symbol, *p);
}
std::optional<double> OrderBookManager::bestAsk(const std::string& symbol) const {
    const OrderBook* b = findBook_(symbol);
    if (!b) return std::nullopt;
    auto p = b->bestAsk(); if (!p) return std::nullopt; return toDouble(symbol, *p);
}
uint64_t OrderBookManager::bestBidSize(const std::string& symbol) const {
    const OrderBook* b = findBook_(symbol);
    return b ? b->bestBidSize() : 0;
}
uint64_t OrderBookManager::bestAskSize(const std::string& symbol) const {
    const OrderBook* b = findBook_(symbol);
    return b ? b->bestAskSize() : 0;
}
void OrderBookManager::depthN(const std::string& symbol, size_t n,
                              std::vector<std::pair<double,uint64_t>>& bids,
                              std::vector<std::pair<double,uint64_t>>& asks) const {
    bids.clear(); asks.clear();
    const OrderBook* b = findBook_(symbol);
    if (!b) return;
    b->forEachLevel(Side::Bid, n, [&](Price p, uint64_t sz){ bids.emplace_back(toDouble(symbol, p), sz); });
    b->forEachLevel(Side::Ask, n, [&](Price p, uint64_t sz){ asks.emplace_back(toDouble(symbol, p), sz); });
}

// ---------- Snapshot builder ----------
DepthSnapshot OrderBookManager::buildSnapshot(const std::string& symbol, size_t levels) const {
    DepthSnapshot snap;
    snap.symbol = symbol;
    {
        std::lock_guard<std::mutex> lk(feedMx_);
        auto it = feed_.find(symbol);
        snap.epoch = (it == feed_.end()) ? 0 : it->second.epoch;
    }
    {
        std::lock_guard<std::mutex> lk(subsMx_);
        snap.seq = pubSeq_.count(symbol) ? pubSeq_.at(symbol) : 0;
    }

    const OrderBook* b = findBook_(symbol);
    if (b) {
        b->forEachLevel(Side::Bid, levels, [&](Price p, uint64_t sz){ snap.bids.emplace_back(p, sz); });
        b->forEachLevel(Side::Ask, levels, [&](Price p, uint64_t sz){ snap.asks.emplace_back(p, sz); });
    }
    return snap;
}

// ---------- Admin / Observability ----------
MetricsSnapshot OrderBookManager::getStats() const { return metrics_.snapshot(); }

bool OrderBookManager::assertInvariants(const std::string& symbol, std::string* whyNot) const {
    const OrderBook* b = findBook_(symbol);
    if (!b) { if (whyNot) *whyNot = "book not found"; return false; }
    return b->checkInvariants(whyNot);
}

std::string OrderBookManager::dumpLadder(const std::string& symbol, size_t maxLevelsPerSide) const {
    std::ostringstream oss;
    oss << "=== DUMP " << symbol << " ===\n";
    {
        std::lock_guard<std::mutex> lk(feedMx_);
        auto it = feed_.find(symbol);
        if (it != feed_.end())
            oss << "epoch=" << it->second.epoch << " stale=" << (it->second.stale ? "true":"false")
                << " feedSeq=" << it->second.lastSeq << "\n";
    }

    const OrderBook* b = findBook_(symbol);

    // Bids
    oss << "[BIDS]\n";
    size_t count = 0;
    if (b) b->forEachLevel(Side::Bid, maxLevelsPerSide, [&](Price p, uint64_t sz){
        oss << std::fixed << std::setprecision(10)
            << "  " << std::setw(12) << toDouble(symbol, p) << "  x " << sz << "\n";
        ++count;
    });
    if (count == 0) oss << "  (empty)\n";

    // Asks
    oss << "[ASKS]\n"; count = 0;
    if (b) b->forEachLevel(Side::Ask, maxLevelsPerSide, [&](Price p, uint64_t sz){
        oss << std::fixed << std::setprecision(10)
            << "  " << std::setw(12) << toDouble(symbol, p) << "  x " << sz << "\n";
        ++count;
    });
    if (count == 0) oss << "  (empty)\n";

    return oss.str();
}

} // namespace gma
