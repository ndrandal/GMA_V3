#include "gma/FunctionMap.hpp"
#include <stdexcept>
#include <mutex>

using namespace gma;

FunctionMap& FunctionMap::instance() {
    static FunctionMap inst;
    return inst;
}

void FunctionMap::registerFunction(const std::string& name, Func f) {
    std::unique_lock lock(_mutex);
    _map[name] = std::move(f);
}

Func FunctionMap::getFunction(const std::string& name) const {
    std::shared_lock lock(_mutex);
    auto it = _map.find(name);
    if (it == _map.end()) throw std::runtime_error("Function not found: " + name);
    return it->second;
}

std::vector<std::pair<std::string, Func>> FunctionMap::getAll() const {
    std::shared_lock lock(_mutex);
    std::vector<std::pair<std::string, Func>> v;
    v.reserve(_map.size());
    for (const auto& kv : _map) {
        v.emplace_back(kv.first, kv.second);
    }
    return v;
}
