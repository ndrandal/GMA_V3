#include "gma/nodes/Responder.hpp"
#include "gma/Logger.hpp"

using namespace gma::nodes;

Responder::Responder(std::function<void(int,const SymbolValue&)> send,
                     int key)
  : _send(std::move(send))
  , _key(key)
{}

void Responder::onValue(const SymbolValue& sv) {
    try {
        _send(_key, sv);
    } catch (const std::exception& ex) {
        Logger::error("Responder send failed: " + std::string(ex.what()));
        shutdown();
    }
}

void Responder::shutdown() noexcept {
    // nothing special to do here anymore
}


