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
    void onValue(const SymbolValue&) override { ++count; }
    void shutdown() noexcept override {}
};

TEST(IntervalTest, PeriodicInvocation) {
    rt::ThreadPool pool(1);
    auto stub = std::make_shared<IStubNode>();
    auto interval = std::make_shared<Interval>(10ms, stub, &pool);
    interval->start();
    std::this_thread::sleep_for(55ms);
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
    std::this_thread::sleep_for(80ms);
    EXPECT_GE(stub1->count.load(), 3) << "i1 should fire more frequently";
    EXPECT_GE(stub2->count.load(), 2) << "i2 should fire at least twice";
    i1->shutdown();
    i2->shutdown();
    pool.shutdown();
}
