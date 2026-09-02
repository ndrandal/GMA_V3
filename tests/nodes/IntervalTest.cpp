#include "gma/nodes/Interval.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

using namespace gma;
using namespace std::chrono_literals;

class IStubNode : public INode {
public:
    std::atomic<int> count{0};
    void onValue(const StreamValue&) override { ++count; }
    void shutdown() noexcept override {}
};

TEST(IntervalTest, PeriodicInvocation) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<IStubNode>();
    auto interval = std::make_shared<Interval>(10ms, stub, &pool);
    interval->start();
    // L18: poll-with-timeout for the expected count instead of asserting after
    // a single fixed sleep. A 10ms interval reaches 3 fires in ~30ms; the
    // generous 2s ceiling makes this robust under load without ever sleeping
    // longer than necessary on a fast machine.
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while (stub->count.load() < 3 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    int cnt = stub->count.load();
    EXPECT_GE(cnt, 3) << "Expected at least 3 invocations, got " << cnt;
    interval->shutdown();
    pool.shutdown();
}

TEST(IntervalTest, ShutdownStopsInvocations) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<IStubNode>();
    auto interval = std::make_shared<Interval>(15ms, stub, &pool);
    interval->start();
    std::this_thread::sleep_for(40ms);
    interval->shutdown();
    int before = stub->count.load();
    std::this_thread::sleep_for(45ms);
    int after = stub->count.load();
    EXPECT_EQ(after, before) << "Count should not increase after shutdown";
    pool.shutdown();
}

TEST(IntervalTest, NoCrashOnZeroDelay) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<IStubNode>();
    auto interval = std::make_shared<Interval>(0ms, stub, &pool);
    interval->start();
    std::this_thread::sleep_for(20ms);
    interval->shutdown();
    pool.shutdown();
    SUCCEED();
}

TEST(IntervalTest, MultipleIntervalsIndependently) {
    rt::ThreadPool pool(2);
    auto stub1 = std::make_shared<IStubNode>();
    auto stub2 = std::make_shared<IStubNode>();
    auto i1 = std::make_shared<Interval>(10ms, stub1, &pool);
    auto i2 = std::make_shared<Interval>(20ms, stub2, &pool);
    i1->start();
    i2->start();
    // L18: poll-with-timeout for both expected counts rather than a single
    // fixed sleep (10ms/20ms intervals reach 3/2 fires in ~40ms; 2s ceiling).
    auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((stub1->count.load() < 3 || stub2->count.load() < 2) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    EXPECT_GE(stub1->count.load(), 3) << "i1 should fire more frequently";
    EXPECT_GE(stub2->count.load(), 2) << "i2 should fire at least twice";
    i1->shutdown();
    i2->shutdown();
    pool.shutdown();
}

// ----- ENC-1065 -----
//
// Dropping the last owning reference must actually destroy the Interval and
// stop its timer thread. The timer thread used to hold its own
// shared_from_this(), so the refcount never reached zero, ~Interval never ran,
// and the thread kept firing into a child, a ThreadPool and a store that the
// test had already torn down.
//
// This is the failure that does not fail where it is caused: in the ENC-1007
// work it surfaced as a SIGFPE in an unrelated suite ~40 suites later. A test
// that passes in isolation proves nothing about it, which is why the assertion
// here is on the lifetime itself, not on downstream symptoms.
TEST(IntervalTest, DestructionWithoutShutdownStopsTheTimerThread) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<IStubNode>();
    std::weak_ptr<Interval> weak;
    {
        auto interval = std::make_shared<Interval>(5ms, stub, &pool);
        weak = interval;
        interval->start();
        // Wait until the timer thread is demonstrably running, so the test is
        // about lifetime and not about a thread that never got going.
        auto deadline = std::chrono::steady_clock::now() + 2s;
        while (stub->count.load() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        ASSERT_GE(stub->count.load(), 2) << "timer never started; test proves nothing";
        // Drop the last owning reference WITHOUT calling shutdown().
    }

    EXPECT_TRUE(weak.expired())
        << "Interval outlived its last owner — the timer thread still holds a "
           "strong self-reference, so ~Interval can never run";

    const int before = stub->count.load();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(stub->count.load(), before)
        << "timer kept firing after the Interval's last owner was dropped";

    pool.shutdown();
}
