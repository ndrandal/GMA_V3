#pragma once
#include <string>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include "gma/nodes/INode.hpp"

namespace gma {
class RequestRegistry {
public:
    void registerRequest(const std::string& id, std::shared_ptr<INode> root);
    void unregisterRequest(const std::string& id);
    void shutdownAll() noexcept;
private:
    std::unordered_map<std::string, std::shared_ptr<INode>> _requests;
    std::shared_mutex _mutex;
};
}
