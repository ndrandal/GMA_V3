// ENC-1280 — the aligned bucket boundary must stop dying as a loop-body local.
//
// SPEC specs/2026-09-20-timestamps-on-the-wire/SPEC.md D9/D10. Both
// wall-clock-aligned timer nodes already compute the boundary they wait on;
// before this change they used it once as a `cv.wait_until` deadline and
// emitted without it, so nothing downstream could say which bar a value
// belonged to. Now the bucket's start rides `StreamValue::bucketStartMs`.
//
// What is pinned here, in order:
//   1. the pure arithmetic, against HARD-CODED known bucket boundaries — the
//      acceptance criterion. No clock is read, so this test cannot be flaky
//      and cannot be satisfied by a wall-clock stamp (D10 forbids one).
//   2. the bridging invariant: the stamp is exactly the floor-aligned bucket
//      CONTAINING the instant the timer node sampled, i.e. the same alignment
//      `nextAlignedAfter` already guarantees, one period earlier.
//   3. TumblingWindow and BucketTime actually stamping it.
//   4. survival across a value-transforming node (VectorReducer), which is
//      what sits between TumblingWindow and the Responder in the canonical
//      bar pipeline.
//   5. the un-bucketed path leaving it at the 0 "no basis" sentinel.
//
// The live-path proof — that this reaches the wire embassy actually reads —
// is `ClientSessionTest.BucketedPipelineStampsBucketStartMsOnTheWire`, which
// drives a real WebSocketServer/ClientSession over a real socket.

#include "gma/FunctionMap.hpp"
#include "gma/FunctionRegistry.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/BucketTime.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/TumblingWindow.hpp"
#include "gma/nodes/VectorReducer.hpp"
#include "gma/rt/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace gma;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

// Records the bucket stamp of every emit, plus the symbol, so the stamping
// tests can assert on a bar's identity without caring about its payload.
class StampRecorder : public INode {
public:
  struct Frame {
    std::string  symbol;
    std::int64_t bucketStartMs;
  };

  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    frames_.push_back(Frame{sv.symbol, sv.bucketStartMs});
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

std::int64_t nowMs() {
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

system_clock::time_point tpFromMs(std::int64_t ms) {
  return system_clock::time_point{milliseconds{ms}};
}

} // namespace

// ---------------------------------------------------------------------------
// 1. The arithmetic, against known bucket boundaries. Clock-free.
// ---------------------------------------------------------------------------

// Each row is (instant, period, expected next boundary, expected bucket start),
// worked out by hand from the wall-clock grid. 1789000012345 ms is
// 2026-09-20T05:46:52.345Z; on a 60 s grid it sits inside the 05:46:00 bar,
// which closes at 05:47:00.
TEST(BucketStartStampTest, BucketStartMsForPinsKnownBoundaries) {
  struct Row {
    std::int64_t fromMs;
    std::int64_t periodMs;
    std::int64_t expectNextMs;
    std::int64_t expectStartMs;
  };
  const Row rows[] = {
    // 1-minute bars around a fixed instant.
    {1789000012345LL, 60000LL, 1789000020000LL, 1788999960000LL},
    // The same instant on a 1 s and a 100 ms grid.
    {1789000012345LL,  1000LL, 1789000013000LL, 1789000012000LL},
    {1789000012345LL,   100LL, 1789000012400LL, 1789000012300LL},
    // Exactly ON a boundary: nextAlignedAfter is strictly-after, so the bar
    // being waited through is the one that STARTS at `from`.
    {1789000020000LL, 60000LL, 1789000080000LL, 1789000020000LL},
  };

  for (const auto& r : rows) {
    const auto period = milliseconds{r.periodMs};
    const auto next   = BucketTime::nextAlignedAfter(tpFromMs(r.fromMs), period);
    const auto nextMs =
        duration_cast<milliseconds>(next.time_since_epoch()).count();
    EXPECT_EQ(nextMs, r.expectNextMs)
        << "nextAlignedAfter(" << r.fromMs << ", " << r.periodMs << ")";

    const std::int64_t startMs = BucketTime::bucketStartMsFor(next, period);
    EXPECT_EQ(startMs, r.expectStartMs)
        << "bucketStartMsFor(" << nextMs << ", " << r.periodMs << ")";

    // The three properties every consumer downstream relies on.
    EXPECT_EQ(startMs % r.periodMs, 0) << "stamp must sit on the period grid";
    EXPECT_EQ(nextMs - startMs, r.periodMs) << "bar must be exactly one period wide";
    EXPECT_LE(startMs, r.fromMs);
    EXPECT_LT(r.fromMs, startMs + r.periodMs)
        << "the stamped bar must CONTAIN the instant the node sampled";
  }
}

// A non-positive period has no bucket grid, so there is no bucket identity to
// report: 0, the StreamValue "not bucketed" sentinel, never a garbage epoch.
// (This mirrors nextAlignedAfter's own defensive branch.)
TEST(BucketStartStampTest, BucketStartMsForRejectsNonPositivePeriod) {
  EXPECT_EQ(BucketTime::bucketStartMsFor(tpFromMs(1789000012345LL), 0ms), 0);
  EXPECT_EQ(BucketTime::bucketStartMsFor(tpFromMs(1789000012345LL), -5ms), 0);
}

// The bridging invariant, swept across a whole period at ms resolution: the
// stamp derived from the boundary is ALWAYS the floor-aligned bucket
// containing the sampled instant. This is the formal statement of "the same
// alignment TumblingWindow already guarantees" — and it is what makes the
// stamp bucket-derived rather than wall-clock-derived (D10).
TEST(BucketStartStampTest, StampIsTheFloorAlignedBucketContainingTheSample) {
  const std::int64_t periodMs = 250;
  const auto period = milliseconds{periodMs};
  const std::int64_t base = 1789000000000LL; // on the 250 ms grid

  for (std::int64_t off = 0; off < periodMs * 3; ++off) {
    const std::int64_t sample = base + off;
    const auto boundary = BucketTime::nextAlignedAfter(tpFromMs(sample), period);
    const std::int64_t startMs = BucketTime::bucketStartMsFor(boundary, period);
    EXPECT_EQ(startMs, (sample / periodMs) * periodMs) << "sample=" << sample;
  }
}

// ---------------------------------------------------------------------------
// 2. The two timer nodes actually stamp it.
// ---------------------------------------------------------------------------

// TumblingWindow's emit carries the bar it aggregated. Asserted against the
// wall clock read either side of the run rather than against a fixed value,
// because the node reads the real clock: the stamp must be grid-aligned, must
// be non-zero, and must name a bar inside the window the test was running.
TEST(BucketStartStampTest, TumblingWindowStampsTheBarItAggregated) {
  const std::int64_t periodMs = 100;

  rt::ThreadPool pool(1);
  auto rec = std::make_shared<StampRecorder>();
  auto tw  = std::make_shared<TumblingWindow>(milliseconds{periodMs}, rec, &pool);
  tw->start();

  const std::int64_t t0 = nowMs();
  for (double v = 1.0; v <= 5.0; v += 1.0)
    tw->onValue(StreamValue{"NEXO", ArgType{v}});
  std::this_thread::sleep_for(350ms);
  const std::int64_t t1 = nowMs();

  tw->shutdown();
  pool.shutdown();

  auto frames = rec->snapshot();
  ASSERT_GE(frames.size(), 1u);
  for (const auto& f : frames) {
    EXPECT_NE(f.bucketStartMs, 0) << "boundary was discarded — the ENC-1280 bug";
    EXPECT_EQ(f.bucketStartMs % periodMs, 0) << "stamp off the period grid";
    // The bar must have closed during the test: it starts no earlier than the
    // bar containing t0, and no later than the bar containing t1.
    EXPECT_GE(f.bucketStartMs, (t0 / periodMs) * periodMs - periodMs);
    EXPECT_LE(f.bucketStartMs, (t1 / periodMs) * periodMs);
  }
}

// BucketTime is a bare clock pulse (StreamValue{"", 0.0}); it now carries the
// identity of the bucket that closed. Consecutive pulses must be exactly one
// period apart on the grid — which is the alignment guarantee the node exists
// for, now observable downstream instead of only inferable from arrival times.
TEST(BucketStartStampTest, BucketTimePulseCarriesConsecutiveAlignedBuckets) {
  const std::int64_t periodMs = 50;

  rt::ThreadPool pool(1);
  auto rec = std::make_shared<StampRecorder>();
  auto bt  = std::make_shared<BucketTime>(milliseconds{periodMs}, rec, &pool);
  bt->start();
  std::this_thread::sleep_for(320ms);
  bt->shutdown();
  pool.shutdown();

  auto frames = rec->snapshot();
  ASSERT_GE(frames.size(), 2u) << "need two pulses to compare bars";
  for (const auto& f : frames) {
    EXPECT_NE(f.bucketStartMs, 0);
    EXPECT_EQ(f.bucketStartMs % periodMs, 0);
  }
  // Strictly increasing, and every gap a whole number of periods. A skipped
  // bar (a late wake-up under load) shows as a gap of 2 periods — the D7
  // behaviour — never as a fractional or negative step.
  for (std::size_t i = 1; i < frames.size(); ++i) {
    const std::int64_t d = frames[i].bucketStartMs - frames[i - 1].bucketStartMs;
    EXPECT_GT(d, 0) << "bars must advance";
    EXPECT_EQ(d % periodMs, 0) << "gap of " << d << "ms is not a whole number of bars";
  }
}

// ---------------------------------------------------------------------------
// 3. Survival across a value-transforming node.
// ---------------------------------------------------------------------------

// VectorReducer builds a NEW StreamValue (TumblingWindow emits a vector, the
// reducer a scalar), so without explicit propagation the stamp would be reset
// to 0 one hop after being computed — and the canonical bar pipeline
// (TumblingWindow -> VectorReducer -> Responder) would put nothing on the
// wire. This is the regression guard for that.
TEST(BucketStartStampTest, StampSurvivesVectorReducer) {
  const std::int64_t periodMs = 100;
  gma::registerBuiltinFunctions();

  rt::ThreadPool pool(1);
  auto rec     = std::make_shared<StampRecorder>();
  auto reducer = std::make_shared<VectorReducer>(
      FunctionMap::instance().getFunction("max"), rec);
  auto tw = std::make_shared<TumblingWindow>(milliseconds{periodMs}, reducer, &pool);
  tw->start();

  const std::int64_t t0 = nowMs();
  for (int i = 1; i <= 5; ++i)
    tw->onValue(StreamValue{"NEXO", ArgType{static_cast<double>(i)}});
  std::this_thread::sleep_for(350ms);
  const std::int64_t t1 = nowMs();

  tw->shutdown();
  pool.shutdown();

  auto frames = rec->snapshot();
  ASSERT_GE(frames.size(), 1u);
  for (const auto& f : frames) {
    EXPECT_NE(f.bucketStartMs, 0) << "VectorReducer dropped the bar identity";
    EXPECT_EQ(f.bucketStartMs % periodMs, 0);
    EXPECT_GE(f.bucketStartMs, (t0 / periodMs) * periodMs - periodMs);
    EXPECT_LE(f.bucketStartMs, (t1 / periodMs) * periodMs);
  }
}

// ---------------------------------------------------------------------------
// 4. The un-bucketed path declares no basis.
// ---------------------------------------------------------------------------

// A StreamValue built the way every non-timer producer builds one (Listener,
// Dispatcher, a raw Responder path) is value-initialised to 0: "no bucket
// identity", which ClientSession renders as an ABSENT wire key rather than a
// 1970 epoch. Also the aggregate-compatibility check — the two-element braced
// init that every one of the 16 production construction sites uses still
// compiles and still means what it meant.
TEST(BucketStartStampTest, UnbucketedStreamValueDeclaresNoBasis) {
  StreamValue sv{"NEXO", ArgType{1.5}};
  EXPECT_EQ(sv.bucketStartMs, 0);

  StreamValue stamped{"NEXO", ArgType{1.5}, 1788999960000LL};
  EXPECT_EQ(stamped.bucketStartMs, 1788999960000LL);
}
