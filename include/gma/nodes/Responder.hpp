#pragma once

#include "gma/nodes/INode.hpp"
#include "gma/StreamValue.hpp"
#include <atomic>
#include <functional>
#include <mutex>

namespace gma::nodes {

/// Sends each StreamValue back to the client via a callback.
class Responder : public INode {
public:
    /// \param send  callback (key, StreamValue) -> void
    /// \param key   the request key to correlate responses
    Responder(std::function<void(int,const StreamValue&)> send,
              int key);

    void onValue(const StreamValue& sv) override;
    void shutdown() noexcept override;

private:
    std::atomic<bool> stopped_{false};
    std::mutex mx_;
    std::function<void(int,const StreamValue&)> send_;
    int key_;
};

} // namespace gma::nodes
