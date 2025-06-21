#include "gma/FunctionMap.hpp"
#include <stdexcept>

using namespace gma;

FunctionMap& FunctionMap::instance() {
    static FunctionMap inst;
    return inst;
}

void FunctionMap::registerFunction(const std::string& name, Func f) {
    std::lock_guard lock(_mutex);
    _map[name] = std::move(f);
}

Func FunctionMap::getFunction(const std::string& name) {
    std::lock_guard lock(_mutex);
    auto it = _map.find(name);
    if (it == _map.end()) throw std::runtime_error("Function not found: " + name);
    return it->second;
}

std::vector<std::pair<std::string, Func>> FunctionMap::getAll() const {
    std::lock_guard lock(_mutex);
    std::vector<std::pair<std::string, Func>> v;
    v.reserve(_map.size());
    for (auto const& kv : _map) {
        v.emplace_back(kv.first, kv.second);
    }
    return v;
}
