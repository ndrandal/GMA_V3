#include "gma/rt/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

using namespace gma;
using namespace gma::rt;

TEST(ThreadPoolTest, ExecutesTasks) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int tasks = 100;
    for (int i = 0; i < tasks; ++i) {
        pool.post([&counter](){ counter++; });
    }
    pool.shutdown();
    EXPECT_EQ(counter.load(), tasks);
}

TEST(ThreadPoolTest, NoDeadlockOnImmediateShutdown) {
    ThreadPool pool(2);
    pool.shutdown();
    SUCCEED();  // No hang implies success
}

TEST(ThreadPoolTest, PostAfterShutdownDoesNothing) {
    ThreadPool pool(2);
    std::atomic<int> counter{0};
    pool.shutdown();
    pool.post([&counter](){ counter++; });
    // Allow brief window for any incorrectly executed tasks
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(counter.load(), 0);
}

TEST(ThreadPoolTest, DestructorShutsDownAndExecutesTasks) {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(3);
        for (int i = 0; i < 50; ++i) {
            pool.post([&counter](){ counter++; });
        }
        // Destructor should call shutdown and complete tasks
    }
    EXPECT_EQ(counter.load(), 50);
}

TEST(ThreadPoolTest, ConcurrentPost) {
    ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int threads = 4, tasksPerThread = 25;
    std::vector<std::thread> posters;
    for (int t = 0; t < threads; ++t) {
        posters.emplace_back([&pool, &counter, tasksPerThread](){
            for (int i = 0; i < tasksPerThread; ++i) {
                pool.post([&counter](){ counter++; });
            }
        });
    }
    for (auto& p : posters) p.join();
    pool.shutdown();
    EXPECT_EQ(counter.load(), threads * tasksPerThread);
}
