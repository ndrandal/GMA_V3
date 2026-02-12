#include <benchmark/benchmark.h>
#include "gma/AtomicStore.hpp"
#include <string>
#include <thread>
#include <vector>

static void BM_AtomicStoreSet_SingleThread(benchmark::State& state) {
    gma::AtomicStore store;
    int i = 0;
    for (auto _ : state) {
        store.set("SYM", "field", static_cast<double>(i++));
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AtomicStoreSet_SingleThread);

static void BM_AtomicStoreGet_SingleThread(benchmark::State& state) {
    gma::AtomicStore store;
    store.set("SYM", "field", 42.0);
    for (auto _ : state) {
        benchmark::DoNotOptimize(store.get("SYM", "field"));
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AtomicStoreGet_SingleThread);

static void BM_AtomicStoreBatchSet(benchmark::State& state) {
    gma::AtomicStore store;
    std::vector<std::pair<std::string, gma::ArgType>> batch;
    for (int i = 0; i < 20; ++i) {
        batch.emplace_back("field_" + std::to_string(i), static_cast<double>(i));
    }
    for (auto _ : state) {
        store.setBatch("SYM", batch);
    }
    state.SetItemsProcessed(state.iterations() * 20);
}

BENCHMARK(BM_AtomicStoreBatchSet);

static void BM_AtomicStoreSet_Contended(benchmark::State& state) {
    static gma::AtomicStore store;
    const int tid = static_cast<int>(state.thread_index());
    const std::string sym = "SYM_" + std::to_string(tid);
    int i = 0;
    for (auto _ : state) {
        store.set(sym, "field", static_cast<double>(i++));
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_AtomicStoreSet_Contended)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

BENCHMARK_MAIN();
