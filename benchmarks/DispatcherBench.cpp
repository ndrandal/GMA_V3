#include <benchmark/benchmark.h>
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/SymbolTick.hpp"
#include "gma/nodes/INode.hpp"
#include <rapidjson/document.h>
#include <memory>

namespace {

class NullNode final : public gma::INode {
public:
    void onValue(const gma::SymbolValue&) override {}
    void shutdown() noexcept override {}
};

static gma::SymbolTick makeTick(const std::string& symbol, double price) {
    auto doc = std::make_shared<rapidjson::Document>();
    doc->SetObject();
    auto& alloc = doc->GetAllocator();
    doc->AddMember("price", rapidjson::Value(price), alloc);
    return gma::SymbolTick{symbol, std::move(doc)};
}

} // namespace

static void BM_DispatcherOnTick(benchmark::State& state) {
    gma::rt::ThreadPool pool(2);
    gma::AtomicStore store;
    gma::MarketDispatcher md(&pool, &store);

    auto listener = std::make_shared<NullNode>();
    md.registerListener("BENCH", "price", listener);

    // Warm up with some history
    for (int i = 0; i < 100; ++i) {
        md.onTick(makeTick("BENCH", 100.0 + i * 0.1));
    }
    pool.drain();

    double price = 200.0;
    for (auto _ : state) {
        md.onTick(makeTick("BENCH", price));
        price += 0.01;
    }
    pool.drain();
    pool.shutdown();

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_DispatcherOnTick)->Unit(benchmark::kMicrosecond);

BENCHMARK_MAIN();
