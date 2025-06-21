#include "gma/Config.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <limits>

using namespace gma;

TEST(ConfigTest, ListenerQueueMaxValue) {
    EXPECT_EQ(Config::ListenerQueueMax, 1000u);
    EXPECT_GT(Config::ListenerQueueMax, 0u);
}

TEST(ConfigTest, HistoryMaxSizeValue) {
    EXPECT_EQ(Config::HistoryMaxSize, 1000u);
    EXPECT_GE(Config::HistoryMaxSize, Config::ListenerQueueMax);
}

TEST(ConfigTest, ThreadPoolSizeValue) {
    EXPECT_EQ(Config::ThreadPoolSize, 4u);
    EXPECT_GT(Config::ThreadPoolSize, 0u);
}

TEST(ConfigTest, ThreadPoolVsQueueSize) {
    EXPECT_LT(Config::ThreadPoolSize, Config::ListenerQueueMax);
}

TEST(ConfigTest, NoOverflowOnQueue) {
    // ensure queue max is within size_t limits
    EXPECT_LT(Config::ListenerQueueMax, std::numeric_limits<std::size_t>::max());
}

TEST(ConfigTest, HardwareConcurrencyNotTooLow) {
    unsigned int hc = std::thread::hardware_concurrency();
    if (hc > 0) {
        EXPECT_LE(Config::ThreadPoolSize, hc) << "ThreadPoolSize should not exceed hardware concurrency";
    } else {
        SUCCEED() << "Hardware concurrency not reported; skipping comparison";
    }
}
