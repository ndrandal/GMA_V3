#include "gma/nodes/Interval.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

using namespace gma;
using namespace std::chrono_literals;

// Stub node to record invocation count
class StubNode : public INode {
public:
    std::atomic<int> count{0};
    void onValue(const SymbolValue& sv) override {
        (void)sv;
        ++count;
    }
    void shutdown() noexcept override {}
};

TEST(IntervalTest, PeriodicInvocation) {
    ThreadPool pool(1);
    auto stub = std::make_shared<StubNode>();
    // 10ms interval
    Interval interval(10ms, stub, &pool);
    // Wait for a few intervals
    std::this_thread::sleep_for(55ms);
    int cnt = stub->count.load();
    EXPECT_GE(cnt, 3) << "Expected at least 3 invocations, got " << cnt;
    interval.shutdown();
    pool.shutdown();
}

TEST(IntervalTest, ShutdownStopsInvocations) {
    ThreadPool pool(1);
    auto stub = std::make_shared<StubNode>();
    Interval interval(15ms, stub, &pool);
    // Allow a couple of invocations
    std::this_thread::sleep_for(40ms);
    interval.shutdown();
    int before = stub->count.load();
    // Wait additional time to ensure no further invocations
    std::this_thread::sleep_for(45ms);
    int after = stub->count.load();
    EXPECT_EQ(after, before) << "Count should not increase after shutdown";
    pool.shutdown();
}

TEST(IntervalTest, NoCrashOnZeroDelay) {
    ThreadPool pool(1);
    auto stub = std::make_shared<StubNode>();
    // Zero delay should still invoke rapidly without crashing
    Interval interval(0ms, stub, &pool);
    std::this_thread::sleep_for(20ms);
    interval.shutdown();
    pool.shutdown();
    SUCCEED();
}

TEST(IntervalTest, MultipleIntervalsIndependently) {
    ThreadPool pool(2);
    auto stub1 = std::make_shared<StubNode>();
    auto stub2 = std::make_shared<StubNode>();
    Interval i1(10ms, stub1, &pool);
    Interval i2(20ms, stub2, &pool);
    std::this_thread::sleep_for(60ms);
    EXPECT_GE(stub1->count.load(), 4) << "i1 should fire more frequently";
    EXPECT_GE(stub2->count.load(), 2) << "i2 should fire at least twice";
    i1.shutdown();
    i2.shutdown();
    pool.shutdown();
}
