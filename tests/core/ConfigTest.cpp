#include "gma/util/Config.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>

using namespace gma::util;

TEST(ConfigTest, DefaultValues) {
    Config cfg;
    EXPECT_EQ(cfg.taMACD_fast, 12);
    EXPECT_EQ(cfg.taMACD_slow, 26);
    EXPECT_EQ(cfg.taBBands_n, 20);
    EXPECT_DOUBLE_EQ(cfg.taBBands_stdK, 2.0);
}

TEST(ConfigTest, LoadFromFile) {
    const char* path = "test_config.ini";
    {
        std::ofstream f(path);
        f << "taMACD_fast=8\n"
          << "taMACD_slow=21\n"
          << "taBBands_n=15\n"
          << "taBBands_stdK=1.5\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 8);
    EXPECT_EQ(cfg.taMACD_slow, 21);
    EXPECT_EQ(cfg.taBBands_n, 15);
    EXPECT_DOUBLE_EQ(cfg.taBBands_stdK, 1.5);
    std::remove(path);
}

TEST(ConfigTest, LoadIgnoresComments) {
    const char* path = "test_config_comments.ini";
    {
        std::ofstream f(path);
        f << "# This is a comment\n"
          << "; So is this\n"
          << "taMACD_fast=5\n"
          << "\n"
          << "taMACD_slow=30\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 5);
    EXPECT_EQ(cfg.taMACD_slow, 30);
    std::remove(path);
}

TEST(ConfigTest, LoadIgnoresUnknownKeys) {
    const char* path = "test_config_unknown.ini";
    {
        std::ofstream f(path);
        f << "unknownKey=999\n"
          << "taMACD_fast=7\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 7);
    // Unknown keys should be silently ignored; defaults preserved for unset keys
    EXPECT_EQ(cfg.taMACD_slow, 26);
    std::remove(path);
}

TEST(ConfigTest, SwapsSlowFastIfInverted) {
    const char* path = "test_config_swap.ini";
    {
        std::ofstream f(path);
        f << "taMACD_fast=30\n"
          << "taMACD_slow=10\n";
    }
    Config cfg;
    cfg.loadFromFile(path);
    EXPECT_LE(cfg.taMACD_fast, cfg.taMACD_slow);
    std::remove(path);
}

TEST(ConfigTest, ReturnsFalseForMissingFile) {
    Config cfg;
    EXPECT_FALSE(cfg.loadFromFile("nonexistent_file_12345.ini"));
}
