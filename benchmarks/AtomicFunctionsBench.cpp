#include <benchmark/benchmark.h>
#include "gma/AtomicFunctions.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolHistory.hpp"
#include <cmath>

static gma::SymbolHistory makeHistory(size_t n) {
    gma::SymbolHistory hist;
    for (size_t i = 0; i < n; ++i) {
        double price = 100.0 + 10.0 * std::sin(static_cast<double>(i) * 0.1);
        double volume = 1000.0 + 500.0 * std::cos(static_cast<double>(i) * 0.05);
        hist.push_back({price, volume});
    }
    return hist;
}

static void BM_ComputeAllAtomicValues(benchmark::State& state) {
    const size_t n = static_cast<size_t>(state.range(0));
    auto hist = makeHistory(n);
    gma::AtomicStore store;

    for (auto _ : state) {
        gma::computeAllAtomicValues("BENCH", hist, store);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ComputeAllAtomicValues)
    ->Arg(50)
    ->Arg(200)
    ->Arg(500)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
