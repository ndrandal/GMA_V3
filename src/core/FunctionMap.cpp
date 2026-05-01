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

void FunctionMap::registerParamFunction(const std::string& name, ParamFunc f) {
    std::unique_lock lock(_mutex);
    _paramMap[name] = std::move(f);
}

Func FunctionMap::getFunction(const std::string& name) const {
    std::shared_lock lock(_mutex);
    auto it = _map.find(name);
    if (it == _map.end()) throw std::runtime_error("Function not found: " + name);
    return it->second;
}

ParamFunc FunctionMap::getParamFunction(const std::string& name) const {
    std::shared_lock lock(_mutex);
    auto it = _paramMap.find(name);
    if (it == _paramMap.end()) throw std::runtime_error("Parametric function not found: " + name);
    return it->second;
}

bool FunctionMap::isParametric(const std::string& name) const {
    std::shared_lock lock(_mutex);
    return _paramMap.find(name) != _paramMap.end();
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
