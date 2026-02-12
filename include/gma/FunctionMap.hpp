#pragma once
#include <string>
#include <functional>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

namespace gma {
using Func = std::function<double(const std::vector<double>&)>;

class FunctionMap {
public:
    static FunctionMap& instance();

    /// Register a new function under `name`.
    void registerFunction(const std::string& name, Func f);

    /// Lookup a function by name. Throws if not found.
    Func getFunction(const std::string& name) const;

    /// Returns a snapshot of all registered [name->Func] pairs.
    std::vector<std::pair<std::string, Func>> getAll() const;

private:
    std::unordered_map<std::string, Func> _map;
    mutable std::shared_mutex _mutex;
};
}
