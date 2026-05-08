#pragma once

#include "gma/nodes/INode.hpp"
#include "gma/server/RequestKey.hpp"
#include "gma/StreamValue.hpp"
#include <atomic>
#include <functional>
#include <mutex>

namespace gma::nodes {

/// Sends each StreamValue back to the client via a callback.
///
/// The request key is a `gma::server::RequestKey` (variant<int,
/// std::string>) so smoke.js's int-keyed wire and embassy's string-id
/// wire are both supported. The callback is responsible for rendering
/// the appropriate JSON field per variant alternative
/// (`writeRequestKeyJSON` is the canonical helper).
class Responder : public INode {
public:
    using SendFn = std::function<void(const gma::server::RequestKey&,
                                      const StreamValue&)>;

    /// \param send  callback (RequestKey, StreamValue) -> void
    /// \param key   the request key to correlate responses
    Responder(SendFn send, gma::server::RequestKey key);

    void onValue(const StreamValue& sv) override;
    void shutdown() noexcept override;

private:
    std::atomic<bool> stopped_{false};
    std::mutex mx_;
    SendFn send_;
    gma::server::RequestKey key_;
};

} // namespace gma::nodes
