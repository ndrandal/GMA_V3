#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include <rapidjson/document.h>

#include "gma/book/OrderBook.hpp"   // Side, Aggressor

namespace gma::feed {

// ---- Canonical events that any feed adapter can produce ----

/// A market-data tick (price, volume, arbitrary numeric fields).
struct TickEvent {
    std::string symbol;
    std::shared_ptr<rapidjson::Document> payload;
    uint64_t timestampNs = 0;   // nanoseconds since epoch (0 = not provided)
};

/// Add an order to the book.
struct ObAddEvent {
    std::string symbol;
    uint64_t    orderId;
    Side        side;
    double      price;
    uint64_t    size;
    uint64_t    priority = 0;
};

/// Update an existing order (price and/or size).
struct ObUpdateEvent {
    std::string              symbol;
    uint64_t                 orderId;
    std::optional<double>    newPrice;
    std::optional<uint64_t>  newSize;
};

/// Delete an order from the book.
struct ObDeleteEvent {
    std::string symbol;
    uint64_t    orderId;
};

/// A trade executed on the venue.
struct ObTradeEvent {
    std::string symbol;
    double      price;
    uint64_t    size;
    Aggressor   aggressor = Aggressor::Unknown;
    uint64_t    timestampNs = 0;   // nanoseconds since epoch (0 = not provided)
};

/// Set the tick size for a symbol.
struct ObTickSizeEvent {
    std::string symbol;
    double      tickSize;
};

/// Reset / new epoch for a symbol's order book.
struct ObResetEvent {
    std::string symbol;
    uint32_t    epoch = 0;
};

/// Union of every event a feed adapter may emit.
using FeedEvent = std::variant<
    TickEvent,
    ObAddEvent,
    ObUpdateEvent,
    ObDeleteEvent,
    ObTradeEvent,
    ObTickSizeEvent,
    ObResetEvent
>;

} // namespace gma::feed
