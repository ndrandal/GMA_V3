#pragma once

#include <string>
#include <map>
#include <vector>
#include <deque>
#include <unordered_map>
#include <shared_mutex>
#include <memory>

#include "gma/SymbolTick.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/SymbolValue.hpp"

namespace gma {

class ThreadPool;
class AtomicStore;

/// Dispatches raw JSON ticks and computed atomic values to symbol/field-scoped listeners.
class MarketDispatcher {
public:
    MarketDispatcher(ThreadPool* threadPool, AtomicStore* store);
    ~MarketDispatcher();

    /// Subscribe a listener to receive raw tick values for a specific symbol/field.
    void registerListener(const std::string& symbol,
                          const std::string& field,
                          std::shared_ptr<INode> listener);

    /// Unsubscribe a listener from raw tick values.
    void unregisterListener(const std::string& symbol,
                            const std::string& field,
                            std::shared_ptr<INode> listener);

    /// Ingest a full JSON tick and dispatch field-specific updates.
    void onTick(const SymbolTick& tick);

    /// Compute all atomic indicators for the given history and notify subscribers.
    void computeAndStoreAtomics(const std::string& symbol,
                                const std::string& field,
                                const std::deque<double>& history);

    /// Retrieve a copy of the history buffer for a given symbol and field.
    std::deque<double> getHistoryCopy(const std::string& symbol,
                                      const std::string& field) const;

private:
    // History buffers per (symbol, field)
    std::unordered_map<
        std::string, // symbol
        std::unordered_map<
            std::string,       // field name
            std::deque<double> // history entries
        >
    > _histories;

    // Listener lists per (symbol, field)
    std::unordered_map<
        std::string, // symbol
        std::map<
            std::string,                     // field name
            std::vector<std::shared_ptr<INode>> // subscribers
        >
    > _listeners;

    mutable std::shared_mutex _mutex;  // protects _histories and _listeners
    ThreadPool* _threadPool;           // for offloading work
    AtomicStore* _store;               // where atomic results are written
    static constexpr size_t MAX_HISTORY = 1000;
};

} // namespace gma
