#include "gma/Logger.hpp"
#include <gtest/gtest.h>
#include <sstream>
#include <iostream>

using namespace gma;

// Helper RAII class to redirect std::cout or std::cerr to an ostringstream
class StreamRedirect {
public:
    StreamRedirect(std::ostream& targetStream, std::ostream& redirectTo)
        : stream_(targetStream), originalBuf_(stream_.rdbuf()) {
        stream_.rdbuf(redirectTo.rdbuf());
    }
    ~StreamRedirect() {
        stream_.rdbuf(originalBuf_);
    }
private:
    std::ostream& stream_;
    std::streambuf* originalBuf_;
};

TEST(LoggerTest, InfoOutputsToStdout) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::info("Hello");

    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(out.str(), "[INFO] Hello\n");
}

TEST(LoggerTest, WarnOutputsToStderr) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::warn("Warning!");

    EXPECT_EQ(out.str(), "");
    EXPECT_EQ(err.str(), "[WARN] Warning!\n");
}

TEST(LoggerTest, ErrorOutputsToStderr) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::error("Error occurred");

    EXPECT_EQ(out.str(), "");
    EXPECT_EQ(err.str(), "[ERROR] Error occurred\n");
}

TEST(LoggerTest, EmptyMessage) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::info("");

    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(out.str(), "[INFO] \n");
}

TEST(LoggerTest, LongMessage) {
    std::string msg(1000, 'x');
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::error(msg);

    EXPECT_EQ(out.str(), "");
    EXPECT_EQ(err.str(), "[ERROR] " + msg + "\n");
}

TEST(LoggerTest, MultipleCallsAccumulate) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::info("One");
    Logger::info("Two");

    EXPECT_EQ(err.str(), "");
    EXPECT_EQ(out.str(), "[INFO] One\n[INFO] Two\n");
}

TEST(LoggerTest, MultiLineMessage) {
    std::ostringstream out;
    std::ostringstream err;
    StreamRedirect redirectOut(std::cout, out);
    StreamRedirect redirectErr(std::cerr, err);

    Logger::warn("Line1\nLine2");

    EXPECT_EQ(out.str(), "");
    // Should prefix only once and preserve the newline within the message
    EXPECT_EQ(err.str(), "[WARN] Line1\nLine2\n");
}
