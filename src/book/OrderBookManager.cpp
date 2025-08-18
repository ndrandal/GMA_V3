#include "gma/book/OrderBookManager.hpp"

namespace gma {

OrderBook& OrderBookManager::book(const std::string& symbol) {
    {
        std::shared_lock rlk(booksMx_);
        auto it = books_.find(symbol);
        if (it != books_.end()) return it->second;
    }
    std::unique_lock wlk(booksMx_);
    return books_.try_emplace(symbol).first->second;
}

bool OrderBookManager::onAdd(const std::string& symbol, const Order& o, FeedScope scope, bool idMissing) {
    return book(symbol).applyAdd(o, scope, idMissing);
}
bool OrderBookManager::onUpdate(const std::string& symbol, const OrderKey& key,
                                std::optional<double> newPrice, std::optional<uint64_t> newSize) {
    return book(symbol).applyUpdate(key, newPrice, newSize);
}
bool OrderBookManager::onDelete(const std::string& symbol, const OrderKey& key) {
    return book(symbol).applyDelete(key);
}



bool OrderBookManager::onTrade(const std::string& symbol, double tradePrice, uint64_t size, Aggressor aggr) {
    return book(symbol).applyTrade(tradePrice, size, aggr);
}

// ---- D3 routing ----

void OrderBookManager::onSnapshotPerOrder(const std::string& symbol, const std::vector<Order>& orders) {
    book(symbol).applySnapshotPerOrder(orders);
}

void OrderBookManager::onSnapshotAggregated(const std::string& symbol, const std::vector<LevelSnapshotEntry>& levels) {
    book(symbol).applySnapshotAggregated(levels);
}

bool OrderBookManager::onLevelSummary(const std::string& symbol, Side side, double price, uint64_t totalSize,
                                      std::optional<uint32_t> orderCount) {
    return book(symbol).applyLevelSummary(side, price, totalSize, orderCount);
}

} // namespace gma
