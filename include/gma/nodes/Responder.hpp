#pragma once

#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include <atomic>
#include <functional>
#include <mutex>

namespace gma::nodes {

/// Sends each SymbolValue back to the client via a callback.
class Responder : public INode {
public:
    /// \param send  callback (key, SymbolValue) -> void
    /// \param key   the request key to correlate responses
    Responder(std::function<void(int,const SymbolValue&)> send,
              int key);

    void onValue(const SymbolValue& sv) override;
    void shutdown() noexcept override;

private:
    std::atomic<bool> stopped_{false};
    std::mutex mx_;
    std::function<void(int,const SymbolValue&)> send_;
    int key_;
};

} // namespace gma::nodes
