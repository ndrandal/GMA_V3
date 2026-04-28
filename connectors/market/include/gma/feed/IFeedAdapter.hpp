#pragma once

#include <string>
#include <vector>

#include "gma/feed/FeedEvent.hpp"

namespace gma::feed {

/// Interface for translating raw (vendor-specific) messages into canonical
/// FeedEvents.  Implement one adapter per data-source protocol (ITCH, Coinbase,
/// FIX, generic JSON, …).
class IFeedAdapter {
public:
    virtual ~IFeedAdapter() = default;

    /// Parse a single raw message and return zero or more canonical events.
    /// A single wire message may produce multiple events (e.g. an ITCH
    /// order_executed yields an OB update *and* a trade tick).
    virtual std::vector<FeedEvent> translate(const std::string& rawMessage) = 0;

    /// Build the subscription message to send on connect.
    /// Override to match vendor-specific subscribe protocols.
    /// Default emits: {"action":"subscribe","symbols":["sym1","sym2",...]}
    virtual std::string subscribeMessage(const std::vector<std::string>& symbols);
};

} // namespace gma::feed
