#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gma {

struct MetricsSnapshot {
    // global counters
    uint64_t adds=0, updates=0, deletes=0, trades=0, priorities=0, summaries=0, snapshots=0;
    uint64_t seqGaps=0, seqResets=0, staleTransitions=0;
    uint64_t droppedMalformed=0, droppedStale=0, deltasPublished=0;

    struct PerSymbol {
        std::string symbol;
        uint64_t deltasPublished=0;
    };
    std::vector<PerSymbol> perSymbol;
};

class Metrics {
public:
    // Increments (global)
    void incAdds()               { adds_.fetch_add(1, std::memory_order_relaxed); }
    void incUpdates()            { updates_.fetch_add(1, std::memory_order_relaxed); }
    void incDeletes()            { deletes_.fetch_add(1, std::memory_order_relaxed); }
    void incTrades()             { trades_.fetch_add(1, std::memory_order_relaxed); }
    void incPriorities()         { priorities_.fetch_add(1, std::memory_order_relaxed); }
    void incSummaries()          { summaries_.fetch_add(1, std::memory_order_relaxed); }
    void incSnapshots()          { snapshots_.fetch_add(1, std::memory_order_relaxed); }

    void incSeqGap()             { seqGaps_.fetch_add(1, std::memory_order_relaxed); }
    void incSeqReset()           { seqResets_.fetch_add(1, std::memory_order_relaxed); }
    void incStaleTransition()    { staleTransitions_.fetch_add(1, std::memory_order_relaxed); }

    void incDroppedMalformed()   { droppedMalformed_.fetch_add(1, std::memory_order_relaxed); }
    void incDroppedStale()       { droppedStale_.fetch_add(1, std::memory_order_relaxed); }

    // Per-symbol delta publication
    void incDeltasPublished(const std::string& symbol) {
        deltasPublished_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mx_);
        perSymDeltas_[symbol] += 1;
    }

    MetricsSnapshot snapshot() const {
        MetricsSnapshot s;
        s.adds             = adds_.load(std::memory_order_relaxed);
        s.updates          = updates_.load(std::memory_order_relaxed);
        s.deletes          = deletes_.load(std::memory_order_relaxed);
        s.trades           = trades_.load(std::memory_order_relaxed);
        s.priorities       = priorities_.load(std::memory_order_relaxed);
        s.summaries        = summaries_.load(std::memory_order_relaxed);
        s.snapshots        = snapshots_.load(std::memory_order_relaxed);
        s.seqGaps          = seqGaps_.load(std::memory_order_relaxed);
        s.seqResets        = seqResets_.load(std::memory_order_relaxed);
        s.staleTransitions = staleTransitions_.load(std::memory_order_relaxed);
        s.droppedMalformed = droppedMalformed_.load(std::memory_order_relaxed);
        s.droppedStale     = droppedStale_.load(std::memory_order_relaxed);
        s.deltasPublished  = deltasPublished_.load(std::memory_order_relaxed);

        std::lock_guard<std::mutex> lk(mx_);
        s.perSymbol.reserve(perSymDeltas_.size());
        for (const auto& kv : perSymDeltas_) s.perSymbol.push_back({kv.first, kv.second});
        return s;
    }

private:
    // global counters
    std::atomic<uint64_t> adds_{0}, updates_{0}, deletes_{0}, trades_{0}, priorities_{0}, summaries_{0}, snapshots_{0};
    std::atomic<uint64_t> seqGaps_{0}, seqResets_{0}, staleTransitions_{0};
    std::atomic<uint64_t> droppedMalformed_{0}, droppedStale_{0}, deltasPublished_{0};

    // per-symbol
    mutable std::mutex mx_;
    std::unordered_map<std::string, uint64_t> perSymDeltas_;
};

} // namespace gma
