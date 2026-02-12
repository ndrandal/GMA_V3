#include "gma/util/Logger.hpp"
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <string>

using namespace gma::util;

TEST(LoggerTest, DefaultLevelIsInfo) {
    Logger lg;
    EXPECT_EQ(lg.level(), LogLevel::Info);
}

TEST(LoggerTest, SetLevelFiltersLowerLevels) {
    Logger lg;
    lg.setLevel(LogLevel::Warn);
    EXPECT_EQ(lg.level(), LogLevel::Warn);
}

TEST(LoggerTest, LogWritesToFile) {
    const char* path = "test_log_output.log";
    Logger lg;
    lg.setFile(path);
    lg.log(LogLevel::Info, "hello world");
    lg.setFile("");  // flush and close

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("hello world"), std::string::npos);
    EXPECT_NE(content.find("INFO"), std::string::npos);
    std::remove(path);
}

TEST(LoggerTest, BelowLevelNotWritten) {
    const char* path = "test_log_filter.log";
    Logger lg;
    lg.setLevel(LogLevel::Error);
    lg.setFile(path);
    lg.log(LogLevel::Info, "should not appear");
    lg.log(LogLevel::Error, "should appear");
    lg.setFile("");

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("should not appear"), std::string::npos);
    EXPECT_NE(content.find("should appear"), std::string::npos);
    std::remove(path);
}

TEST(LoggerTest, JsonFormatProducesJson) {
    const char* path = "test_log_json.log";
    Logger lg;
    lg.setFormatJson(true);
    lg.setFile(path);
    lg.log(LogLevel::Warn, "test msg", {{"key", "val"}});
    lg.setFile("");

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("{"), std::string::npos);
    EXPECT_NE(content.find("\"msg\""), std::string::npos);
    EXPECT_NE(content.find("\"key\""), std::string::npos);
    std::remove(path);
}

TEST(LoggerTest, FieldsAppearInOutput) {
    const char* path = "test_log_fields.log";
    Logger lg;
    lg.setFile(path);
    lg.log(LogLevel::Info, "msg", {{"port", "8080"}, {"host", "localhost"}});
    lg.setFile("");

    std::ifstream f(path);
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("port"), std::string::npos);
    EXPECT_NE(content.find("8080"), std::string::npos);
    std::remove(path);
}

TEST(LoggerTest, ParseLevelRoundTrips) {
    EXPECT_EQ(parseLevel("trace"), LogLevel::Trace);
    EXPECT_EQ(parseLevel("debug"), LogLevel::Debug);
    EXPECT_EQ(parseLevel("info"),  LogLevel::Info);
    EXPECT_EQ(parseLevel("warn"),  LogLevel::Warn);
    EXPECT_EQ(parseLevel("error"), LogLevel::Error);
    EXPECT_EQ(parseLevel("UNKNOWN"), LogLevel::Info);
}

TEST(LoggerTest, GlobalLoggerIsSingleton) {
    Logger& a = logger();
    Logger& b = logger();
    EXPECT_EQ(&a, &b);
}
