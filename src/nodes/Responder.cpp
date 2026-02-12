#include "gma/nodes/Responder.hpp"
#include "gma/util/Logger.hpp"

using namespace gma::nodes;

Responder::Responder(std::function<void(int,const SymbolValue&)> send,
                     int key)
  : send_(std::move(send))
  , key_(key)
{}

void Responder::onValue(const SymbolValue& sv) {
    if (!send_) return;
    try {
        send_(key_, sv);
    } catch (const std::exception& ex) {
        gma::util::logger().log(gma::util::LogLevel::Error,
                                "Responder send failed",
                                { {"err", ex.what()} });
    }
}

void Responder::shutdown() noexcept {
    send_ = nullptr;
}
