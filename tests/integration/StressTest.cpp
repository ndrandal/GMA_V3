#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

using namespace gma;

TEST(StressTest, ConcurrentAtomicStoreWrites) {
    AtomicStore store;
    const int threads = 4;
    const int writes = 1000;
    std::vector<std::thread> ths;

    for (int t = 0; t < threads; ++t) {
        ths.emplace_back([&store, t, writes]() {
            for (int i = 0; i < writes; ++i) {
                store.set("SYM", "field_" + std::to_string(t), static_cast<double>(i));
            }
        });
    }
    for (auto& th : ths) th.join();

    // Each thread wrote its own field; final value should be writes-1
    for (int t = 0; t < threads; ++t) {
        auto val = store.get("SYM", "field_" + std::to_string(t));
        ASSERT_TRUE(val.has_value());
        EXPECT_DOUBLE_EQ(std::get<double>(val.value()), static_cast<double>(writes - 1));
    }
}

TEST(StressTest, ThreadPoolHighConcurrency) {
    rt::ThreadPool pool(4);
    std::atomic<int> counter{0};
    const int tasks = 10000;

    for (int i = 0; i < tasks; ++i) {
        pool.post([&counter]() { counter++; });
    }

    pool.shutdown();
    EXPECT_EQ(counter.load(), tasks);
}
