#include "gma/ta/TAComputer.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace gma::ta;

TEST(TAComputerTest, SetAndGetLastPrice) {
    TAComputer tc;
    tc.setLastPrice("AAPL", 150.0);
    EXPECT_DOUBLE_EQ(tc.getLastPrice("AAPL"), 150.0);
}

TEST(TAComputerTest, GetLastPriceThrowsOnMissing) {
    TAComputer tc;
    EXPECT_THROW(tc.getLastPrice("MISSING"), std::out_of_range);
}

TEST(TAComputerTest, GetStateReturnsCopy) {
    TAComputer tc;
    tc.setLastPrice("AAPL", 100.0);
    auto state = tc.getState("AAPL");
    EXPECT_DOUBLE_EQ(state.lastPrice, 100.0);

    // Modifying the copy should not affect the original.
    state.lastPrice = 999.0;
    EXPECT_DOUBLE_EQ(tc.getLastPrice("AAPL"), 100.0);
}

TEST(TAComputerTest, GetStateThrowsOnMissing) {
    TAComputer tc;
    EXPECT_THROW(tc.getState("MISSING"), std::out_of_range);
}

TEST(TAComputerTest, HasSymbol) {
    TAComputer tc;
    EXPECT_FALSE(tc.has("AAPL"));
    tc.setLastPrice("AAPL", 50.0);
    EXPECT_TRUE(tc.has("AAPL"));
}

TEST(TAComputerTest, ConcurrentSetLastPriceIsSafe) {
    TAComputer tc;
    constexpr int N = 8;
    constexpr int ITERS = 1000;

    std::vector<std::thread> threads;
    threads.reserve(N);
    for (int t = 0; t < N; ++t) {
        threads.emplace_back([&tc, t]() {
            std::string sym = "SYM_" + std::to_string(t);
            for (int i = 0; i < ITERS; ++i) {
                tc.setLastPrice(sym, static_cast<double>(i));
            }
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < N; ++t) {
        std::string sym = "SYM_" + std::to_string(t);
        EXPECT_DOUBLE_EQ(tc.getLastPrice(sym), static_cast<double>(ITERS - 1));
    }
}
