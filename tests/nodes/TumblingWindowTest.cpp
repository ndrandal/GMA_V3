// Tests for gma::TumblingWindow — per-symbol scalar accumulator with
// wall-clock-aligned tumbling-window emit semantics. See
// include/gma/nodes/TumblingWindow.hpp.
//
// Timing notes: TumblingWindow aligns its boundary to wall-clock multiples
// of `period`, so a test that starts mid-period may have to wait < period
// for the *first* boundary and then `period` again for the second. To
// reliably catch ≥ 1 emit, every timing-sensitive test sleeps `period * 3`
// or more and asserts with `EXPECT_GE` where the exact count would be
// fragile. Emit dispatch is async on the ThreadPool, so tests also tolerate
// a small post-boundary settle delay before reading the recorder.

#include "gma/nodes/TumblingWindow.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/StreamValue.hpp"
#include <gtest/gtest.h>
#include <atomic>
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

// RecorderNode captures every (symbol, vector<double>) emit. Thread-safe:
// TumblingWindow dispatches via ThreadPool::post, so onValue lands on a
// worker thread, and tests read from the main thread.
class RecorderNode : public INode {
public:
  struct Frame { std::string symbol; std::vector<double> values; };

  void onValue(const StreamValue& sv) override {
    Frame f;
    f.symbol = sv.symbol;
    if (auto* v = std::get_if<std::vector<double>>(&sv.value)) {
      f.values = *v;
    }
    std::lock_guard<std::mutex> lk(mx_);
    frames_.push_back(std::move(f));
  }

  void shutdown() noexcept override {}

  std::vector<Frame> snapshot() const {
    std::lock_guard<std::mutex> lk(mx_);
    return frames_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mx_);
    return frames_.size();
  }

private:
  mutable std::mutex mx_;
  std::vector<Frame> frames_;
};

} // namespace

// Boundary fires and emits the accumulated values for the symbol that
// received them. The exact frame count is timing-dependent (could be 1 or
// 2 depending on alignment) — assert ≥ 1 and inspect the union of values.
TEST(TumblingWindowTest, EmitsAtBoundaryWithAccumulatedValues) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<RecorderNode>();
  auto tw  = std::make_shared<TumblingWindow>(100ms, rec, &pool);
  tw->start();

  for (double v = 1.0; v <= 5.0; v += 1.0) {
    tw->onValue(StreamValue{"NEXO", ArgType{v}});
  }
  std::this_thread::sleep_for(350ms);

  auto frames = rec->snapshot();
  ASSERT_GE(frames.size(), 1u);

  // Total values across all emits should be 5 (every pushed value lands in
  // *some* bucket — could be split if the first sleep crossed a boundary).
  std::size_t total = 0;
  for (auto& f : frames) {
    EXPECT_EQ(f.symbol, "NEXO");
    total += f.values.size();
  }
  EXPECT_EQ(total, 5u);

  tw->shutdown();
  pool.shutdown();
}

// No upstream traffic for 5 boundaries → 0 emits.
TEST(TumblingWindowTest, EmptyBucketDoesNotEmit) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<RecorderNode>();
  auto tw  = std::make_shared<TumblingWindow>(50ms, rec, &pool);
  tw->start();

  std::this_thread::sleep_for(300ms); // ≥ 5 boundaries
  EXPECT_EQ(rec->size(), 0u);

  tw->shutdown();
  pool.shutdown();
}

// Per-symbol independence: values for symbol A and B accumulate separately
// and each non-empty bucket produces one frame per symbol per boundary.
TEST(TumblingWindowTest, MultipleSymbolsBucketIndependently) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<RecorderNode>();
  auto tw  = std::make_shared<TumblingWindow>(100ms, rec, &pool);
  tw->start();

  tw->onValue(StreamValue{"A", ArgType{1.0}});
  tw->onValue(StreamValue{"A", ArgType{2.0}});
  tw->onValue(StreamValue{"B", ArgType{10.0}});
  std::this_thread::sleep_for(350ms);

  auto frames = rec->snapshot();
  ASSERT_GE(frames.size(), 2u);

  std::size_t a_total = 0, b_total = 0;
  bool a_in_one_frame = false, b_in_one_frame = false;
  for (auto& f : frames) {
    if (f.symbol == "A") {
      a_total += f.values.size();
      if (f.values.size() == 2 && f.values[0] == 1.0 && f.values[1] == 2.0)
        a_in_one_frame = true;
    } else if (f.symbol == "B") {
      b_total += f.values.size();
      if (f.values.size() == 1 && f.values[0] == 10.0)
        b_in_one_frame = true;
    }
  }
  EXPECT_EQ(a_total, 2u);
  EXPECT_EQ(b_total, 1u);
  // The pushes happened before the first boundary, so each symbol's values
  // should land in exactly one frame each (per-symbol independence).
  EXPECT_TRUE(a_in_one_frame);
  EXPECT_TRUE(b_in_one_frame);

  tw->shutdown();
  pool.shutdown();
}

// 8 threads × 250 onValue calls each = 2000 values; after a boundary the
// recorder's total value count must equal 2000 (no leak across boundaries,
// no lost updates).
TEST(TumblingWindowTest, ConcurrentOnValueDoesNotLoseUpdates) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<RecorderNode>();
  auto tw  = std::make_shared<TumblingWindow>(200ms, rec, &pool);
  tw->start();

  constexpr int N_THREADS = 8;
  constexpr int N_PER     = 250;
  std::vector<std::thread> writers;
  writers.reserve(N_THREADS);
  for (int t = 0; t < N_THREADS; ++t) {
    writers.emplace_back([tw, t] {
      for (int i = 0; i < N_PER; ++i) {
        tw->onValue(StreamValue{"X", ArgType{static_cast<double>(t * 1000 + i)}});
      }
    });
  }
  for (auto& w : writers) w.join();

  // Wait long enough for ≥ 1 boundary after all writes are visible.
  std::this_thread::sleep_for(500ms);

  std::size_t total = 0;
  for (auto& f : rec->snapshot()) total += f.values.size();
  EXPECT_EQ(total, static_cast<std::size_t>(N_THREADS * N_PER));

  tw->shutdown();
  pool.shutdown();
}

// Lifecycle: 100 construct/start/shutdown cycles back-to-back (matches
// SPEC AC-6). The test passes if it returns at all — a leaked / un-joined
// timer thread would either hang here or trip ASAN/TSAN on teardown.
TEST(TumblingWindowTest, ShutdownJoinsTimerThread) {
  rt::ThreadPool pool(1);
  for (int i = 0; i < 100; ++i) {
    auto rec = std::make_shared<RecorderNode>();
    auto tw  = std::make_shared<TumblingWindow>(50ms, rec, &pool);
    tw->start();
    tw->shutdown();
  }
  pool.shutdown();
  SUCCEED();
}

// ----- ENC-1080 (sibling of ENC-1065, which fixed the same bug on Interval) -----
//
// Dropping the last owning reference must actually destroy the TumblingWindow
// and stop its timer thread. The timer thread used to hold its own
// shared_from_this(), so the refcount never reached zero, ~TumblingWindow
// never ran, and the thread kept flushing buckets into a downstream node, a
// ThreadPool and a store that the test had already torn down.
//
// This is the failure that does not fail where it is caused: in the ENC-1007
// work the Interval instance of it surfaced as a SIGFPE in an unrelated suite
// ~40 suites later. A test that passes in isolation proves nothing about it,
// which is why the primary assertion here is on the lifetime itself, not on a
// downstream symptom.
//
// The secondary assertion is timed deliberately: the test feeds a fresh bucket
// immediately after observing an emit (so the next boundary is nearly a full
// period away), then drops the owner. A leaked timer thread would flush that
// pending bucket at the next boundary; a destroyed one never can.
TEST(TumblingWindowTest, DestructionWithoutShutdownStopsTheTimerThread) {
  rt::ThreadPool pool(1);
  auto rec = std::make_shared<RecorderNode>();
  std::weak_ptr<TumblingWindow> weak;
  {
    auto tw = std::make_shared<TumblingWindow>(100ms, rec, &pool);
    weak = tw;
    tw->start();
    // Wait until two boundaries have actually emitted, so the test is about
    // lifetime and not about a thread that never got going — and so we know
    // we are sitting just *after* a boundary.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (rec->size() < 2 && std::chrono::steady_clock::now() < deadline) {
      tw->onValue(StreamValue{"SYM", 1.0});
      std::this_thread::sleep_for(2ms);
    }
    ASSERT_GE(rec->size(), 2u) << "timer never emitted; test proves nothing";

    // Leave a full bucket pending for the next boundary.
    for (int i = 0; i < 5; ++i) tw->onValue(StreamValue{"SYM", 2.0});
    // Drop the last owning reference WITHOUT calling shutdown().
  }

  EXPECT_TRUE(weak.expired())
      << "TumblingWindow outlived its last owner — the timer thread still "
         "holds a strong self-reference, so ~TumblingWindow can never run";

  // Let any emit already posted to the pool land before sampling.
  std::this_thread::sleep_for(20ms);
  const std::size_t before = rec->size();
  std::this_thread::sleep_for(350ms); // ≥ 3 further boundaries
  EXPECT_EQ(rec->size(), before)
      << "timer kept flushing after the TumblingWindow's last owner was dropped";

  pool.shutdown();
}
