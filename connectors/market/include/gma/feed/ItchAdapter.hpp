#pragma once

#include "gma/feed/IFeedAdapter.hpp"
#include "gma/book/OrderBook.hpp"   // Side

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

    /// Build a TickEvent with lastPrice + volume fields for TA computation.
    static TickEvent makeTradeTickEvent(const std::string& symbol,
                                        double price, uint64_t size);

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
    std::unordered_map<uint64_t, OrderState> orders_;
};

} // namespace gma::feed
