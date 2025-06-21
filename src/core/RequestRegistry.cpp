#include "gma/RequestRegistry.hpp"
#include "gma/Logger.hpp"
using namespace gma;

void RequestRegistry::registerRequest(const std::string& id, std::shared_ptr<INode> root) {
    std::unique_lock lock(_mutex);
    _requests[id] = root;
}

void RequestRegistry::unregisterRequest(const std::string& id) {
    std::shared_ptr<INode> root;
    {
        std::unique_lock lock(_mutex);
        auto it = _requests.find(id);
        if (it == _requests.end()) return;
        root = it->second;
        _requests.erase(it);
    }
    if (root) {
        Logger::info("Shutting down request " + id);
        root->shutdown();
    }
}
void RequestRegistry::shutdownAll() noexcept
{
    std::unique_lock lock(_mutex);
    for (auto& [id, node] : _requests) {
        if (node) {
            // each node implements shutdown()
            node->shutdown();
        }
    }
    _requests.clear();
}