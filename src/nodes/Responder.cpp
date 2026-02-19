#include "gma/nodes/Responder.hpp"
#include "gma/util/Logger.hpp"

using namespace gma::nodes;

Responder::Responder(std::function<void(int,const SymbolValue&)> send,
                     int key)
  : send_(std::move(send))
  , key_(key)
{}

void Responder::onValue(const SymbolValue& sv) {
    if (stopped_.load(std::memory_order_acquire)) return;

    // Copy send_ under lock to avoid TOCTOU race with shutdown().
    std::function<void(int,const SymbolValue&)> fn;
    {
        std::lock_guard<std::mutex> lk(mx_);
        fn = send_;
    }
    if (!fn) return;

    try {
        fn(key_, sv);
    } catch (const std::exception& ex) {
        gma::util::logger().log(gma::util::LogLevel::Error,
                                "Responder send failed",
                                { {"err", ex.what()} });
    }
}

void Responder::shutdown() noexcept {
    stopped_.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lk(mx_);
    send_ = nullptr;
}
