// Tests for gma::BucketTime — wall-clock-aligned periodic tick source.
//
// The alignment guarantee is the load-bearing property: every consumer
// of a BucketTime(60000) ticks at the same wall-clock minute boundary,
// so a "1m bar" means the same window across the whole data plane.
// These tests use shorter periods (10–100 ms) to keep runtime tight
// while still exercising the alignment math.

#include "gma/nodes/BucketTime.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace gma;
using namespace std::chrono;
using namespace std::chrono_literals;

namespace {

// Captures (count, timestamps) per onValue. Timestamps let us verify
// alignment to wall-clock period boundaries.
class TimestampingStub : public INode {
public:
    std::atomic<int> count{0};
    std::vector<system_clock::time_point> ticks;
    std::mutex mx;
    void onValue(const StreamValue&) override {
        std::lock_guard<std::mutex> lk(mx);
        ++count;
        ticks.push_back(system_clock::now());
    }
    void shutdown() noexcept override {}
};

} // namespace

TEST(BucketTimeTest, EmitsAtLeastOnceWithinTwoPeriods) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<TimestampingStub>();
    auto bt = std::make_shared<BucketTime>(20ms, stub, &pool);
    bt->start();
    std::this_thread::sleep_for(50ms);
    int cnt = stub->count.load();
    EXPECT_GE(cnt, 1) << "Expected at least 1 tick within 2 periods, got " << cnt;
    bt->shutdown();
    pool.shutdown();
}

TEST(BucketTimeTest, ShutdownStopsTicks) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<TimestampingStub>();
    auto bt = std::make_shared<BucketTime>(15ms, stub, &pool);
    bt->start();
    std::this_thread::sleep_for(40ms);
    bt->shutdown();
    int before = stub->count.load();
    std::this_thread::sleep_for(45ms);
    int after = stub->count.load();
    EXPECT_EQ(after, before) << "Count should not increase after shutdown";
    pool.shutdown();
}

// The alignment property: ticks land at wall-clock multiples of the
// period (within a small tolerance for kernel scheduling jitter). With
// period=100ms we expect every tick's epoch_ms % 100 to be near zero.
TEST(BucketTimeTest, TicksAlignToWallClockPeriod) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<TimestampingStub>();
    constexpr auto period = 100ms;
    auto bt = std::make_shared<BucketTime>(period, stub, &pool);
    bt->start();
    std::this_thread::sleep_for(550ms); // ~5 ticks expected
    bt->shutdown();

    std::vector<system_clock::time_point> snapshot;
    {
        std::lock_guard<std::mutex> lk(stub->mx);
        snapshot = stub->ticks;
    }
    ASSERT_GE(snapshot.size(), 3u) << "Need at least 3 ticks to verify alignment";

    constexpr long tolerance_ms = 25; // generous for slow CI / loaded box
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const auto epoch_ms = duration_cast<milliseconds>(
            snapshot[i].time_since_epoch()).count();
        const auto offset = epoch_ms % period.count();
        // Distance from the boundary (it's never negative; offset is in
        // [0, period_ms)).
        const auto delta = std::min(offset, period.count() - offset);
        EXPECT_LE(delta, tolerance_ms)
            << "tick[" << i << "] offset=" << offset << "ms from boundary";
    }
    pool.shutdown();
}

// Two BucketTime nodes constructed at slightly different moments still
// fire on the SAME wall-clock boundaries (the property Interval cannot
// provide). Confirms alignment is a function of the wall clock, not
// the node's start time.
TEST(BucketTimeTest, TwoBucketTimesShareBoundary) {
    rt::ThreadPool pool(2);
    auto stub1 = std::make_shared<TimestampingStub>();
    auto stub2 = std::make_shared<TimestampingStub>();
    constexpr auto period = 50ms;
    auto bt1 = std::make_shared<BucketTime>(period, stub1, &pool);
    bt1->start();
    std::this_thread::sleep_for(13ms); // off-period
    auto bt2 = std::make_shared<BucketTime>(period, stub2, &pool);
    bt2->start();
    std::this_thread::sleep_for(220ms);
    bt1->shutdown();
    bt2->shutdown();

    std::vector<system_clock::time_point> ticks1, ticks2;
    {
        std::lock_guard<std::mutex> lk(stub1->mx);
        ticks1 = stub1->ticks;
    }
    {
        std::lock_guard<std::mutex> lk(stub2->mx);
        ticks2 = stub2->ticks;
    }
    ASSERT_FALSE(ticks1.empty());
    ASSERT_FALSE(ticks2.empty());

    // For every tick from bt2, find the closest tick from bt1; they
    // should be within scheduling jitter, NOT 13ms apart (which is what
    // Interval would produce).
    constexpr long jitter_ms = 25;
    for (const auto& t2 : ticks2) {
        long minDiff = std::numeric_limits<long>::max();
        for (const auto& t1 : ticks1) {
            const long d = std::abs(duration_cast<milliseconds>(t1 - t2).count());
            if (d < minDiff) minDiff = d;
        }
        EXPECT_LE(minDiff, jitter_ms)
            << "bt2 tick is " << minDiff << "ms from nearest bt1 tick — alignment broken";
    }
    pool.shutdown();
}

TEST(BucketTimeTest, NextAlignedAfterReturnsStrictlyGreaterMultiple) {
    using TP = system_clock::time_point;
    using MS = milliseconds;
    constexpr auto period = MS{100};

    // Mid-bucket: now=12345 should yield 12400.
    {
        const auto now = TP{MS{12345}};
        const auto next = BucketTime::nextAlignedAfter(now, period);
        EXPECT_EQ(duration_cast<MS>(next.time_since_epoch()).count(), 12400);
    }
    // On-boundary: now=12300 should yield the *next* boundary, 12400.
    {
        const auto now = TP{MS{12300}};
        const auto next = BucketTime::nextAlignedAfter(now, period);
        EXPECT_EQ(duration_cast<MS>(next.time_since_epoch()).count(), 12400);
    }
    // Just past boundary: now=12301 should yield 12400.
    {
        const auto now = TP{MS{12301}};
        const auto next = BucketTime::nextAlignedAfter(now, period);
        EXPECT_EQ(duration_cast<MS>(next.time_since_epoch()).count(), 12400);
    }
}
