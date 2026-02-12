#include <benchmark/benchmark.h>
#include "gma/rt/ThreadPool.hpp"
#include <atomic>

static void BM_ThreadPoolPost(benchmark::State& state) {
    gma::rt::ThreadPool pool(static_cast<unsigned>(state.range(0)));
    std::atomic<int> counter{0};

    for (auto _ : state) {
        pool.post([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    pool.drain();
    pool.shutdown();

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ThreadPoolPost)->Arg(1)->Arg(2)->Arg(4)->Unit(benchmark::kNanosecond);

static void BM_ThreadPoolPostAndDrain(benchmark::State& state) {
    gma::rt::ThreadPool pool(static_cast<unsigned>(state.range(0)));
    std::atomic<int> counter{0};

    for (auto _ : state) {
        for (int i = 0; i < 100; ++i) {
            pool.post([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        pool.drain();
    }
    pool.shutdown();

    state.SetItemsProcessed(state.iterations() * 100);
}

BENCHMARK(BM_ThreadPoolPostAndDrain)->Arg(1)->Arg(2)->Arg(4)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
