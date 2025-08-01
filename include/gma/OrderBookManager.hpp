// include/gma/OrderBookManager.hpp
#pragma once

#include <map>
#include <deque>
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <cstdint>
#include <rapidjson/document.h>

namespace gma {

struct Order {
    uint64_t id;
    char side;             // 'A' = ask, 'B' = bid
    double price;
    uint32_t size;
    uint64_t priority;
};

struct PriceLevel {
    double price;
    std::deque<Order> orders;
    uint64_t totalSize = 0;
};

class OrderBook {
public:
    void applyAdd(const Order&);
    void applyUpdate(const Order&);
    void applyDelete(uint64_t orderId, char side);
    void applyTrade(double price, uint32_t size);
private:
    std::map<double, PriceLevel, std::greater<>> bids_;
    std::map<double, PriceLevel            > asks_;
    std::unordered_map<uint64_t, Order*>   orderById_;
    std::shared_mutex                      mutex_;
};

class OrderBookManager {
public:
    static OrderBookManager& instance();

    /// Dispatches an L2 JSON message ('0','3','4','5','6') to the right symbol book
    void process(const rapidjson::Document& msg);

private:
    OrderBookManager() = default;
    std::unordered_map<std::string, OrderBook> books_;
    std::shared_mutex                          managerMutex_;
};

} // namespace gma
