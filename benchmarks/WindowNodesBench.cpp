// Microbenchmarks for the two windowing nodes (Phase 2 / SPEC AC-5).
// VectorReducer.onValue: vector<double> → scalar via a FunctionMap fn.
// TumblingWindow.onValue: per-symbol scalar push into the accumulator.

#include <benchmark/benchmark.h>
#include "gma/FunctionMap.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/TumblingWindow.hpp"
#include "gma/nodes/VectorReducer.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/StreamValue.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace {

// Counting sink: counts emits, never copies, never allocates. The bench
// uses this instead of stubBroadcaster/RecorderNode (which retain frames)
// so the measurement isolates the node under test, not the sink.
class CountingSink : public gma::INode {
public:
  std::atomic<uint64_t> count{0};
  void onValue(const gma::StreamValue&) override { count.fetch_add(1, std::memory_order_relaxed); }
  void shutdown() noexcept override {}
};

// One-time bootstrap so FunctionMap has "max"/"sum"/etc. registered. Each
// bench main starts with an empty registry; gma::registerBuiltinFunctions
// is idempotent on duplicates.
void ensureBuiltins() {
  static bool done = false;
  if (!done) { gma::registerBuiltinFunctions(); done = true; }
}

} // namespace

// ---------- VectorReducer ----------

static void BM_VectorReducer_Max(benchmark::State& state) {
  ensureBuiltins();
  auto sink = std::make_shared<CountingSink>();
  auto fn   = gma::FunctionMap::instance().getFunction("max");
  gma::VectorReducer vr(fn, sink);
  const std::vector<double> bucket{1.0, 5.0, 3.0, 7.0, 2.0, 6.0, 4.0};
  // Build the StreamValue once outside the timed loop — we want to measure
  // VectorReducer's onValue, not the variant/vector construction. The
  // value is copied into the StreamValue.value variant, but the underlying
  // vector buffer is freshly constructed each iteration to model real
  // upstream traffic (TumblingWindow emits a new vector per boundary).
  for (auto _ : state) {
    gma::StreamValue sv{"NEXO", gma::ArgType{bucket}};
    vr.onValue(sv);
    benchmark::DoNotOptimize(sink->count.load());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorReducer_Max);

static void BM_VectorReducer_Sum(benchmark::State& state) {
  ensureBuiltins();
  auto sink = std::make_shared<CountingSink>();
  auto fn   = gma::FunctionMap::instance().getFunction("sum");
  gma::VectorReducer vr(fn, sink);
  const std::vector<double> bucket{1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};
  for (auto _ : state) {
    gma::StreamValue sv{"NEXO", gma::ArgType{bucket}};
    vr.onValue(sv);
    benchmark::DoNotOptimize(sink->count.load());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_VectorReducer_Sum);

// ---------- TumblingWindow ----------

// Steady-state onValue: after a warm-up phase that pre-grows the per-
// symbol buffer to a stable capacity, every subsequent push should be
// alloc-free (just a lock + back-insert into a vector with spare cap).
// Period is set to 1 hour so no boundary fires during the bench window.
static void BM_TumblingWindow_OnValueSteadyState(benchmark::State& state) {
  auto sink = std::make_shared<CountingSink>();
  gma::rt::ThreadPool pool(1);
  auto tw = std::make_shared<gma::TumblingWindow>(std::chrono::hours(1), sink, &pool);
  tw->start();

  // Warm-up: push enough values so the per-symbol vector reaches its
  // steady-state capacity. Standard libc++ doubling means 16+ pushes
  // settles the capacity into the right power-of-two bucket for the
  // ~10-100 range of typical period contents.
  for (int i = 0; i < 64; ++i) {
    tw->onValue(gma::StreamValue{"NEXO", gma::ArgType{static_cast<double>(i)}});
  }

  double i = 0.0;
  for (auto _ : state) {
    tw->onValue(gma::StreamValue{"NEXO", gma::ArgType{i}});
    i += 1.0;
    benchmark::DoNotOptimize(i);
  }
  state.SetItemsProcessed(state.iterations());

  tw->shutdown();
  pool.shutdown();
}
BENCHMARK(BM_TumblingWindow_OnValueSteadyState);

BENCHMARK_MAIN();
