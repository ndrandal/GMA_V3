#pragma once
#include "gma/book/OrderBook.hpp"
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <vector>

namespace gma {

class OrderBookManager {
public:
    OrderBook& book(const std::string& symbol);

    // Add with scope; set idMissing=true when feed omits/NULLs the order id
    bool onAdd(const std::string& symbol, const Order& o, FeedScope scope, bool idMissing);

    // Scoped update/delete: take OrderKey directly
    bool onUpdate(const std::string& symbol, const OrderKey& key,
                std::optional<double> newPrice, std::optional<uint64_t> newSize);
    bool onDelete(const std::string& symbol, const OrderKey& key);


    // Trades (per-order book consumption)
    bool onTrade(const std::string& symbol, double tradePrice, uint64_t size, Aggressor aggr = Aggressor::Unknown);

    // ---- D3: Snapshot/Summary routing ----
    void onSnapshotPerOrder(const std::string& symbol, const std::vector<Order>& orders);
    void onSnapshotAggregated(const std::string& symbol, const std::vector<LevelSnapshotEntry>& levels);
    bool onLevelSummary(const std::string& symbol, Side side, double price, uint64_t totalSize,
                        std::optional<uint32_t> orderCount = std::nullopt);

private:
    std::unordered_map<std::string, OrderBook> books_;
    mutable std::shared_mutex booksMx_;
};

} // namespace gma
