// Tests for gma::VectorReducer — vector<double> → scalar double via
// FunctionMap. See include/gma/nodes/VectorReducer.hpp.
//
// VectorReducer is synchronous (no timer / no thread pool dispatch), so
// these tests can read the recorder immediately after onValue returns —
// no sleep needed.

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
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

using namespace gma;

namespace {

// Captures scalar emits (StreamValue with double-valued variant).
class ScalarRecorder : public INode {
public:
  struct Frame { std::string symbol; double value; };

  void onValue(const StreamValue& sv) override {
    Frame f;
    f.symbol = sv.symbol;
    if (auto* d = std::get_if<double>(&sv.value)) {
      f.value = *d;
    } else {
      f.value = 0.0 / 0.0; // NaN — sentinel for "didn't carry a double"
    }
    std::lock_guard<std::mutex> lk(mx_);
    frames_.push_back(f);
  }

  void shutdown() noexcept override {}

  std::vector<Frame> snapshot() const {
    std::lock_guard<std::mutex> lk(mx_);
    return frames_;
  }

private:
  mutable std::mutex mx_;
  std::vector<Frame> frames_;
};

} // namespace

// Max over a vector emits the max as a scalar.
TEST(VectorReducerTest, MaxOverVectorEmitsScalar) {
  auto rec = std::make_shared<ScalarRecorder>();
  auto fn  = FunctionMap::instance().getFunction("max");
  VectorReducer vr(fn, rec);

  vr.onValue(StreamValue{"NEXO", ArgType{std::vector<double>{1.0, 5.0, 3.0}}});

  auto frames = rec->snapshot();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].symbol, "NEXO");
  EXPECT_DOUBLE_EQ(frames[0].value, 5.0);
}

// Sum over a vector emits the total.
TEST(VectorReducerTest, SumOverVectorEmitsScalar) {
  auto rec = std::make_shared<ScalarRecorder>();
  auto fn  = FunctionMap::instance().getFunction("sum");
  VectorReducer vr(fn, rec);

  vr.onValue(StreamValue{"VALT", ArgType{std::vector<double>{1.0, 2.0, 3.0, 4.0}}});

  auto frames = rec->snapshot();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].symbol, "VALT");
  EXPECT_DOUBLE_EQ(frames[0].value, 10.0);
}

// Non-vector inputs are dropped (logged Warn, no downstream emit). Tested
// shape: an int-typed StreamValue (what a Worker or Listener would emit).
// VectorReducer is wired to consume TumblingWindow output; a non-vector
// in is a wiring mistake.
TEST(VectorReducerTest, NonVectorInputDropped) {
  auto rec = std::make_shared<ScalarRecorder>();
  auto fn  = FunctionMap::instance().getFunction("max");
  VectorReducer vr(fn, rec);

  vr.onValue(StreamValue{"NEXO", ArgType{42}});      // int
  vr.onValue(StreamValue{"NEXO", ArgType{3.14}});    // double scalar
  vr.onValue(StreamValue{"NEXO", ArgType{std::string{"oops"}}});

  EXPECT_EQ(rec->snapshot().size(), 0u);
}

// Unknown fn rejected at TreeBuilder build time with a clear message.
// Mirrors Worker's unknown-fn build-time-error pattern at
// TreeBuilder.cpp:148-158. ClientSession's catch chain surfaces this to
// the WS peer as `{"type":"error","where":"validate", ...}`.
TEST(VectorReducerTest, UnknownFnRejectedAtBuildTime) {
  // TreeBuilder needs deps.pool for sibling nodes; supply a tiny one.
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher disp(&pool, &store);
  tree::Deps deps;
  deps.pool = &pool;
  deps.store = &store;
  deps.dispatcher = &disp;

  // Terminal is irrelevant — the builder throws before it runs.
  struct NullNode : INode {
    void onValue(const StreamValue&) override {}
    void shutdown() noexcept override {}
  };
  auto terminal = std::make_shared<NullNode>();

  rapidjson::Document spec;
  spec.Parse(R"({"type":"VectorReducer","fn":"bogus-not-registered"})");
  ASSERT_FALSE(spec.HasParseError());

  EXPECT_THROW(tree::buildOne(spec, "NEXO", deps, terminal), std::runtime_error);

  pool.shutdown();
}
