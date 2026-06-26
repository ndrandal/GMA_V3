#include "gma/feed/ItchAdapter.hpp"

#include "gma/util/Logger.hpp"
#include "gma/util/Metrics.hpp"

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <cstdlib>
#include <type_traits>
#include <variant>

namespace gma::feed {

// ---------------------------------------------------------------------------
// translate — entry point: parse JSON, route by ITCH message type
// ---------------------------------------------------------------------------
std::vector<FeedEvent> ItchAdapter::translate(const std::string& rawMessage) {
    rapidjson::Document doc;
    doc.Parse(rawMessage.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return {};

    if (!doc.HasMember("type") || !doc["type"].IsString()) return {};

    std::vector<FeedEvent> out;
    const std::string type = doc["type"].GetString();

    if      (type == "trade")               routeTrade(doc, out);
    else if (type == "add_order")           routeAddOrder(doc, out);
    else if (type == "add_order_mpid")      routeAddOrder(doc, out);
    else if (type == "order_executed")      routeOrderExecuted(doc, out);
    else if (type == "order_cancel")        routeOrderCancel(doc, out);
    else if (type == "order_delete")        routeOrderDelete(doc, out);
    else if (type == "order_replace")       routeOrderReplace(doc, out);
    else if (type == "stock_directory")     routeStockDirectory(doc, out);
    else if (type == "system_event")        routeSystemEvent(doc);
    // stock_trading_action — future: halt/resume

    // M7: stamp an optional per-message wire sequence onto every order-book
    // mutation event so the ingress can gap-detect via OrderBookManager::onSeq.
    // The ITCH JSON used today carries no "seq" field, so this is a no-op
    // unless a sequence-bearing feed is connected (conservative).
    if (doc.HasMember("seq") && doc["seq"].IsUint64()) {
        const uint64_t seq = doc["seq"].GetUint64();
        for (auto& evt : out) {
            std::visit([seq](auto& e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, ObAddEvent>    ||
                              std::is_same_v<T, ObUpdateEvent> ||
                              std::is_same_v<T, ObDeleteEvent> ||
                              std::is_same_v<T, ObTradeEvent>) {
                    e.seq = seq;
                }
            }, evt);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
double ItchAdapter::parsePrice(const rapidjson::Value& v) {
    if (v.IsString()) return std::strtod(v.GetString(), nullptr);
    if (v.IsNumber()) return v.GetDouble();
    return 0.0;
}

TickEvent ItchAdapter::makeTradeTickEvent(const std::string& symbol,
                                           double price, uint64_t size) {
    auto payload = std::make_shared<rapidjson::Document>();
    payload->SetObject();
    auto& a = payload->GetAllocator();
    payload->AddMember("symbol",
        rapidjson::Value(symbol.c_str(), a), a);
    payload->AddMember("lastPrice", price, a);
    payload->AddMember("volume", static_cast<double>(size), a);

    TickEvent te;
    te.symbol  = symbol;
    te.payload = std::move(payload);
    return te;
}

// ---------------------------------------------------------------------------
// stock_directory → tick-size + register symbol mapping
// ---------------------------------------------------------------------------
void ItchAdapter::routeStockDirectory(const rapidjson::Value& doc,
                                       std::vector<FeedEvent>& out) {
    if (!doc.HasMember("stockLocate") || !doc["stockLocate"].IsInt() ||
        !doc.HasMember("stock") || !doc["stock"].IsString()) return;

    int locate = doc["stockLocate"].GetInt();
    std::string stock = doc["stock"].GetString();

    // Trim trailing spaces (ITCH pads to 8 chars)
    while (!stock.empty() && stock.back() == ' ') stock.pop_back();

    locateToSymbol_[locate] = stock;

    // NASDAQ uses $0.01 tick size for all equities
    out.push_back(ObTickSizeEvent{stock, 0.01});

    GMA_METRIC_HIT("feed_ws.stock_directory");

    gma::util::logger().log(gma::util::LogLevel::Info,
        "ItchAdapter.stockDirectory",
        {{"locate", std::to_string(locate)}, {"stock", stock}});
}

// ---------------------------------------------------------------------------
// add_order / add_order_mpid → OB add + track order state
// ---------------------------------------------------------------------------
void ItchAdapter::routeAddOrder(const rapidjson::Value& doc,
                                 std::vector<FeedEvent>& out) {
    if (!doc.HasMember("orderRef") || !doc["orderRef"].IsUint64() ||
        !doc.HasMember("side") || !doc["side"].IsString() ||
        !doc.HasMember("shares") || !doc["shares"].IsUint64() ||
        !doc.HasMember("price")) return;

    // Resolve symbol: prefer "stock" field, fall back to stockLocate mapping
    std::string stock;
    if (doc.HasMember("stock") && doc["stock"].IsString()) {
        stock = doc["stock"].GetString();
        while (!stock.empty() && stock.back() == ' ') stock.pop_back();
    } else if (doc.HasMember("stockLocate") && doc["stockLocate"].IsInt()) {
        auto it = locateToSymbol_.find(doc["stockLocate"].GetInt());
        if (it == locateToSymbol_.end()) return;
        stock = it->second;
    } else {
        return;
    }

    uint64_t orderRef = doc["orderRef"].GetUint64();
    uint64_t shares   = doc["shares"].GetUint64();
    double   price    = parsePrice(doc["price"]);

    const char* sideStr = doc["side"].GetString();
    Side side = (sideStr[0] == 'B') ? Side::Bid : Side::Ask;

    // M4: bound the tracked-order map. orders_ is erased on delete/full-fill/
    // full-cancel, but a feed of orders never subsequently removed would grow
    // it without limit. Evict the oldest bucket entry once the cap is hit;
    // dropping tracking only forfeits resolution of a later partial-fill/cancel
    // for that (pathologically old) order — memory stays bounded.
    if (orders_.size() >= kMaxTrackedOrders && orders_.find(orderRef) == orders_.end()) {
        orders_.erase(orders_.begin());
    }

    // Track order state for executed/cancel messages
    orders_[orderRef] = {stock, shares, side, price};

    out.push_back(ObAddEvent{stock, orderRef, side, price, shares, 0});

    GMA_METRIC_HIT("feed_ws.add_order");
}

// ---------------------------------------------------------------------------
// order_executed → reduce size or delete, emit trade tick
// ---------------------------------------------------------------------------
void ItchAdapter::routeOrderExecuted(const rapidjson::Value& doc,
                                      std::vector<FeedEvent>& out) {
    if (!doc.HasMember("orderRef") || !doc["orderRef"].IsUint64() ||
        !doc.HasMember("shares") || !doc["shares"].IsUint64()) return;

    uint64_t orderRef   = doc["orderRef"].GetUint64();
    uint64_t execShares = doc["shares"].GetUint64();

    auto it = orders_.find(orderRef);
    if (it == orders_.end()) return;

    auto& os = it->second;
    const std::string& symbol = os.symbol;

    // The resting order's side is passive; the aggressor is the opposite.
    Aggressor aggr = (os.side == Side::Bid) ? Aggressor::Sell : Aggressor::Buy;

    // C1: the explicit delete/update below already mutates the book to reflect
    // the fill. The trade event is emitted print-only (bookNeutral) so it does
    // NOT consume the book a second time — the TA/print signal is the separate
    // makeTradeTickEvent tick.
    ObTradeEvent trade{symbol, os.price, execShares, aggr};
    trade.bookNeutral = true;

    if (execShares >= os.remainingShares) {
        // Fully filled — delete the order and erase tracking state
        out.push_back(ObDeleteEvent{symbol, orderRef});
        out.push_back(trade);
        out.push_back(makeTradeTickEvent(symbol, os.price, execShares));
        orders_.erase(it);
    } else {
        // Partial fill — reduce size, keep tracking
        os.remainingShares -= execShares;
        out.push_back(ObUpdateEvent{symbol, orderRef, std::nullopt, os.remainingShares});
        out.push_back(trade);
        out.push_back(makeTradeTickEvent(symbol, os.price, execShares));
    }

    GMA_METRIC_HIT("feed_ws.order_executed");
}

// ---------------------------------------------------------------------------
// order_cancel → partial cancel (reduce remaining shares)
// ---------------------------------------------------------------------------
void ItchAdapter::routeOrderCancel(const rapidjson::Value& doc,
                                    std::vector<FeedEvent>& out) {
    if (!doc.HasMember("orderRef") || !doc["orderRef"].IsUint64() ||
        !doc.HasMember("shares") || !doc["shares"].IsUint64()) return;

    uint64_t orderRef    = doc["orderRef"].GetUint64();
    uint64_t cancelShares = doc["shares"].GetUint64();

    auto it = orders_.find(orderRef);
    if (it == orders_.end()) return;

    auto& os = it->second;

    if (cancelShares >= os.remainingShares) {
        out.push_back(ObDeleteEvent{os.symbol, orderRef});
        orders_.erase(it);
    } else {
        os.remainingShares -= cancelShares;
        out.push_back(ObUpdateEvent{os.symbol, orderRef,
                                     std::nullopt, os.remainingShares});
    }

    GMA_METRIC_HIT("feed_ws.order_cancel");
}

// ---------------------------------------------------------------------------
// order_delete → full removal
// ---------------------------------------------------------------------------
void ItchAdapter::routeOrderDelete(const rapidjson::Value& doc,
                                    std::vector<FeedEvent>& out) {
    if (!doc.HasMember("orderRef") || !doc["orderRef"].IsUint64()) return;

    uint64_t orderRef = doc["orderRef"].GetUint64();

    auto it = orders_.find(orderRef);
    if (it == orders_.end()) return;

    out.push_back(ObDeleteEvent{it->second.symbol, orderRef});
    orders_.erase(it);

    GMA_METRIC_HIT("feed_ws.order_delete");
}

// ---------------------------------------------------------------------------
// order_replace → delete old + add new
// ---------------------------------------------------------------------------
void ItchAdapter::routeOrderReplace(const rapidjson::Value& doc,
                                     std::vector<FeedEvent>& out) {
    if (!doc.HasMember("origOrderRef") || !doc["origOrderRef"].IsUint64() ||
        !doc.HasMember("orderRef") || !doc["orderRef"].IsUint64() ||
        !doc.HasMember("shares") || !doc["shares"].IsUint64() ||
        !doc.HasMember("price")) return;

    uint64_t origRef = doc["origOrderRef"].GetUint64();
    uint64_t newRef  = doc["orderRef"].GetUint64();
    uint64_t shares  = doc["shares"].GetUint64();
    double   price   = parsePrice(doc["price"]);

    auto it = orders_.find(origRef);
    if (it == orders_.end()) return;

    std::string symbol = it->second.symbol;
    Side side = it->second.side;

    // Delete old
    out.push_back(ObDeleteEvent{symbol, origRef});
    orders_.erase(it);

    // Add new
    orders_[newRef] = {symbol, shares, side, price};
    out.push_back(ObAddEvent{symbol, newRef, side, price, shares, 0});

    GMA_METRIC_HIT("feed_ws.order_replace");
}

// ---------------------------------------------------------------------------
// trade → OB trade + dispatcher tick
// ---------------------------------------------------------------------------
void ItchAdapter::routeTrade(const rapidjson::Value& doc,
                              std::vector<FeedEvent>& out) {
    if (!doc.HasMember("price") ||
        !doc.HasMember("shares") || !doc["shares"].IsUint64()) return;

    // Resolve symbol: prefer "stock" field, fall back to stockLocate mapping
    std::string stock;
    if (doc.HasMember("stock") && doc["stock"].IsString()) {
        stock = doc["stock"].GetString();
        while (!stock.empty() && stock.back() == ' ') stock.pop_back();
    } else if (doc.HasMember("stockLocate") && doc["stockLocate"].IsInt()) {
        auto it = locateToSymbol_.find(doc["stockLocate"].GetInt());
        if (it == locateToSymbol_.end()) return;
        stock = it->second;
    } else {
        return;
    }

    double   price  = parsePrice(doc["price"]);
    uint64_t shares = doc["shares"].GetUint64();

    Aggressor aggr = Aggressor::Unknown;
    if (doc.HasMember("side") && doc["side"].IsString()) {
        const char* s = doc["side"].GetString();
        if (s[0] == 'B') aggr = Aggressor::Buy;
        else if (s[0] == 'S') aggr = Aggressor::Sell;
    }

    // M1: a top-level ITCH `trade` print is a non-displayable/hidden execution.
    // It must NOT consume displayed resting liquidity, so emit it print-only
    // (bookNeutral). The TA/print signal is the separate trade tick below.
    ObTradeEvent trade{stock, price, shares, aggr};
    trade.bookNeutral = true;
    out.push_back(trade);

    // Emit tick to dispatcher for TA computation
    out.push_back(makeTradeTickEvent(stock, price, shares));

    GMA_METRIC_HIT("feed_ws.trade");
}

// ---------------------------------------------------------------------------
// system_event — log only
// ---------------------------------------------------------------------------
void ItchAdapter::routeSystemEvent(const rapidjson::Value& doc) {
    std::string code = "?";
    if (doc.HasMember("eventCode") && doc["eventCode"].IsString())
        code = doc["eventCode"].GetString();

    gma::util::logger().log(gma::util::LogLevel::Info,
        "ItchAdapter.systemEvent",
        {{"eventCode", code}});
}

} // namespace gma::feed
