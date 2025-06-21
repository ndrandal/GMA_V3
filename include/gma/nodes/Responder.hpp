#pragma once

#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"
#include <functional>

namespace gma::nodes {

/// Sends each SymbolValue back to the client via a callback.
class Responder : public INode {
public:
    /// \param send  the callback (_key, SymbolValue) → void
    /// \param key   the request key to correlate responses
    Responder(std::function<void(int,const SymbolValue&)> send,
              int key);

    void onValue(const SymbolValue& sv) override;
    void shutdown() noexcept override;

private:
    std::function<void(int,const SymbolValue&)> _send;
    int _key;
};

} // namespace gma::nodes
