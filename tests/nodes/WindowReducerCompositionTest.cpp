// Integration: VectorReducer ← TumblingWindow end-to-end. Drives scalars
// into TumblingWindow, sleeps past a boundary, asserts the recorder
// downstream of VectorReducer received a scalar equal to the reducer
// applied to the bucket contents.
//
// Also asserts TreeBuilder::buildForRequest accepts the equivalent JSON
// config — proving the pipeline-stage wiring (no `input` key — flat
// `pipeline:[...]` array, reverse-iterated) resolves both new node types.

#include "gma/nodes/TumblingWindow.hpp"
#include "gma/nodes/VectorReducer.hpp"
#include "gma/FunctionMap.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/StreamValue.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace gma;
using namespace std::chrono_literals;

namespace {

class ScalarRecorder : public INode {
public:
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    if (auto* d = std::get_if<double>(&sv.value)) {
      values_.push_back(*d);
    }
  }
  void shutdown() noexcept override {}
  std::vector<double> snapshot() const {
    std::lock_guard<std::mutex> lk(mx_);
    return values_;
  }
private:
  mutable std::mutex mx_;
  std::vector<double> values_;
};

} // namespace

// End-to-end: 10 values pushed in one window → VectorReducer(max) → 5.0.
// Period = 100ms; we sleep 350ms to reliably catch ≥ 1 boundary.
TEST(WindowReducerCompositionTest, MaxOverWindowEmitsExpectedScalar) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<ScalarRecorder>();
  auto fn  = FunctionMap::instance().getFunction("max");
  auto reducer = std::make_shared<VectorReducer>(fn, rec);
  auto window  = std::make_shared<TumblingWindow>(100ms, reducer, &pool);
  window->start();

  // 10 values, max = 5.0 (each value appears twice; the second copies are
  // also included so the bucket has 10 entries total).
  for (int i = 1; i <= 5; ++i) {
    window->onValue(StreamValue{"NEXO", ArgType{static_cast<double>(i)}});
    window->onValue(StreamValue{"NEXO", ArgType{static_cast<double>(i)}});
  }

  std::this_thread::sleep_for(350ms);

  auto values = rec->snapshot();
  ASSERT_GE(values.size(), 1u);
  // Every emitted scalar in this run must be the max of *that* bucket's
  // contents. With all 10 pushes landing before the first boundary, the
  // entire batch goes into one bucket → one emit with max=5.0. If the
  // alignment split the batch across two boundaries (rare but possible),
  // each emit is still the max of its bucket, and 5.0 must appear in
  // *some* frame (the one containing the i=5 push).
  bool saw_5 = false;
  for (double v : values) {
    EXPECT_LE(v, 5.0);
    EXPECT_GE(v, 1.0);
    if (v == 5.0) saw_5 = true;
  }
  EXPECT_TRUE(saw_5);

  window->shutdown();
  reducer->shutdown();
  pool.shutdown();
}

// The same shape via TreeBuilder JSON: a Listener clock with a
// pipeline of TumblingWindow → VectorReducer must build to a non-null
// head without exceptions. We don't drive data through this chain (that
// path needs a Dispatcher + real feed event) — the assertion is "the
// JSON shape resolves end-to-end through the node registries."
TEST(WindowReducerCompositionTest, TreeBuilderResolvesComposedJson) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher disp(&pool, &store);
  tree::Deps deps;
  deps.pool = &pool;
  deps.store = &store;
  deps.dispatcher = &disp;

  struct NullNode : INode {
    void onValue(const StreamValue&) override {}
    void shutdown() noexcept override {}
  };
  auto terminal = std::make_shared<NullNode>();

  // Listener-on-bare-key + pipeline of TumblingWindow → VectorReducer.
  // The reverse-iteration in buildForRequest wires each stage's downstream.
  const char* json = R"({
    "streamKey": "NEXO",
    "field": "lastPrice",
    "pipeline": [
      { "type": "TumblingWindow", "periodMs": 1000 },
      { "type": "VectorReducer", "fn": "max" }
    ]
  })";
  rapidjson::Document spec;
  spec.Parse(json);
  ASSERT_FALSE(spec.HasParseError());

  auto chain = tree::buildForRequest(spec, deps, terminal);
  EXPECT_NE(chain.head, nullptr);
  EXPECT_GE(chain.keepAlive.size(), 3u); // terminal + reducer + window (+ listener head)

  pool.shutdown();
}
