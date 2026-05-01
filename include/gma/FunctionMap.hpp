#pragma once
#include <string>
#include <functional>
#include <map>
#include <unordered_map>
#include <shared_mutex>
#include <vector>

namespace gma {
using Func = std::function<double(const std::vector<double>&)>;

/// Parametric function: receives the ordered numeric inputs AND a map of
/// named numeric parameters extracted from the JSON node spec (e.g.
/// `factor` for `scale`). Lets callers register reducers that need
/// per-call tunables without growing dedicated TreeBuilder branches.
using ParamFunc = std::function<double(const std::vector<double>&,
                                       const std::map<std::string, double>&)>;

class FunctionMap {
public:
    static FunctionMap& instance();

    /// Register a new plain reducer under `name`.
    void registerFunction(const std::string& name, Func f);

    /// Register a parametric reducer under `name`. Lookup happens via
    /// getParamFunction(); plain getFunction() will not surface it.
    void registerParamFunction(const std::string& name, ParamFunc f);

    /// Lookup a plain reducer by name. Throws if not registered (or is
    /// only registered as a parametric variant — use getParamFunction).
    Func getFunction(const std::string& name) const;

    /// Lookup a parametric reducer by name. Throws if not registered.
    ParamFunc getParamFunction(const std::string& name) const;

    /// Returns true if `name` was registered as a parametric reducer.
    bool isParametric(const std::string& name) const;

    /// Returns a snapshot of all registered plain [name->Func] pairs.
    std::vector<std::pair<std::string, Func>> getAll() const;

    /// Iterate all registered plain reducers under shared_lock without copying.
    /// Callback signature: void(const std::string& name, const Func& fn)
    template <typename Callback>
    void forEach(Callback&& cb) const {
        std::shared_lock lock(_mutex);
        for (const auto& kv : _map) {
            cb(kv.first, kv.second);
        }
    }

private:
    std::unordered_map<std::string, Func>      _map;
    std::unordered_map<std::string, ParamFunc> _paramMap;
    mutable std::shared_mutex _mutex;
};
}
