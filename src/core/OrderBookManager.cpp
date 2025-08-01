// src/core/OrderBookManager.cpp
#include "gma/OrderBookManager.hpp"
#include <rapidjson/document.h>
#include <numeric>
#include <algorithm>

namespace gma {

OrderBookManager& OrderBookManager::instance() {
    static OrderBookManager mgr;
    return mgr;
}

void OrderBookManager::process(const rapidjson::Document& msg) {
    if (!msg.HasMember("Message Type") || !msg["Message Type"].IsString()) {
        return;
    }
    char type = msg["Message Type"].GetString()[0];
    if (!msg.HasMember("Symbol") || !msg["Symbol"].IsString()) {
        return;
    }
    std::string symbol = msg["Symbol"].GetString();

    // Ensure book exists
    {
        std::shared_lock readLock(managerMutex_);
        if (books_.find(symbol) == books_.end()) {
            readLock.unlock();
            std::unique_lock writeLock(managerMutex_);
            books_.emplace(symbol, OrderBook());
        }
    }
    OrderBook& book = books_.at(symbol);

    switch (type) {
        case '3': { // Add
            Order o{};
            o.id       = msg["Order ID"].GetUint64();
            o.side     = msg["Side"].GetString()[0];
            o.price    = msg["Price"].GetDouble();
            o.size     = msg["Order Size"].GetUint();
            o.priority = msg["Order Priority"].GetUint64();
            book.applyAdd(o);
            break;
        }
        case '4': // Update
        case '6': { // Summary treated as update
            Order o{};
            o.id       = msg["Order ID"].GetUint64();
            o.side     = msg["Side"].GetString()[0];
            o.price    = msg["Price"].GetDouble();
            o.size     = msg["Order Size"].GetUint();
            o.priority = msg["Order Priority"].GetUint64();
            book.applyUpdate(o);
            break;
        }
        case '5': { // Delete
            uint64_t id   = msg["Order ID"].GetUint64();
            char side     = msg["Side"].GetString()[0];
            book.applyDelete(id, side);
            break;
        }
        case '0': { // Price Level Order (snapshot)
            Order o{};
            o.id       = msg["Order ID"].GetUint64();
            o.side     = msg["Side"].GetString()[0];
            o.price    = msg["Price"].GetDouble();
            o.size     = msg["Order Size"].GetUint();
            o.priority = msg["Order Priority"].GetUint64();
            book.applyAdd(o);
            break;
        }
        default:
            // Unknown type: ignore
            break;
    }
}

void OrderBook::applyAdd(const Order& o) {
    std::unique_lock lock(mutex_);
    auto& levels = (o.side == 'B' ? bids_ : asks_);
    auto it = levels.find(o.price);
    if (it == levels.end()) {
        PriceLevel pl{ o.price, {}, 0 };
        auto res = levels.emplace(o.price, std::move(pl));
        it = res.first;
    }
    it->second.orders.push_back(o);
    it->second.totalSize += o.size;
    orderById_[o.id] = &it->second.orders.back();
}

void OrderBook::applyUpdate(const Order& o) {
    std::unique_lock lock(mutex_);
    auto itOrd = orderById_.find(o.id);
    if (itOrd == orderById_.end()) {
        return;
    }
    Order* ord = itOrd->second;
    uint32_t oldSize = ord->size;
    ord->size     = o.size;
    ord->priority = o.priority;
    auto& levels = (o.side == 'B' ? bids_ : asks_);
    auto itLevel = levels.find(o.price);
    if (itLevel != levels.end()) {
        itLevel->second.totalSize += (o.size - oldSize);
    }
}

void OrderBook::applyDelete(uint64_t orderId, char side) {
    std::unique_lock lock(mutex_);
    auto itOrd = orderById_.find(orderId);
    if (itOrd == orderById_.end()) {
        return;
    }
    Order* ord = itOrd->second;
    double price    = ord->price;
    uint32_t size    = ord->size;
    auto& levels    = (side == 'B' ? bids_ : asks_);
    auto itLevel    = levels.find(price);
    if (itLevel != levels.end()) {
        auto& dq = itLevel->second.orders;
        dq.erase(std::remove_if(dq.begin(), dq.end(),
                    [&](const Order& o){ return o.id == orderId; }),
                dq.end());
        itLevel->second.totalSize -= size;
        if (dq.empty()) {
            levels.erase(itLevel);
        }
    }
    orderById_.erase(itOrd);
}

void OrderBook::applyTrade(double price, uint32_t size) {
    std::unique_lock lock(mutex_);
    auto processSide = [&](auto& levels) {
        if (levels.empty() || levels.begin()->first != price) return;
        auto& pl = levels.begin()->second;
        uint32_t rem = size;
        while (rem > 0 && !pl.orders.empty()) {
            Order& front = pl.orders.front();
            if (front.size <= rem) {
                rem -= front.size;
                orderById_.erase(front.id);
                pl.orders.pop_front();
            } else {
                front.size -= rem;
                rem = 0;
            }
        }
        pl.totalSize = std::accumulate(pl.orders.begin(), pl.orders.end(), uint64_t{0},
            [](uint64_t acc, const Order& o){ return acc + o.size; });
        if (pl.orders.empty()) {
            levels.erase(levels.begin());
        }
    };
    processSide(bids_);
    processSide(asks_);
}

} // namespace gma
