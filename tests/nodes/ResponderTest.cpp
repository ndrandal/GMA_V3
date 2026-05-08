#include "gma/nodes/Responder.hpp"
#include "gma/server/RequestKey.hpp"
#include "gma/StreamValue.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <variant>

using namespace gma;
using namespace gma::nodes;
using gma::server::RequestKey;
using gma::server::requestKeyInt;
using gma::server::requestKeyStr;

TEST(ResponderTest, CallsCallbackWithCorrectKeyAndValue) {
    RequestKey capturedKey;
    StreamValue capturedSv;
    auto callback = [&](const RequestKey& key, const StreamValue& sv) {
        capturedKey = key;
        capturedSv = sv;
    };
    Responder responder(callback, requestKeyInt(42));
    StreamValue sv{"ABC", 3.14};
    responder.onValue(sv);
    ASSERT_EQ(capturedKey.index(), 0u);
    EXPECT_EQ(std::get<int>(capturedKey), 42);
    EXPECT_EQ(capturedSv.symbol, "ABC");
    EXPECT_DOUBLE_EQ(std::get<double>(capturedSv.value), 3.14);
}

TEST(ResponderTest, MultipleInvocations) {
    int count = 0;
    auto callback = [&](const RequestKey& key, const StreamValue& sv) {
        ASSERT_EQ(key.index(), 0u);
        EXPECT_EQ(std::get<int>(key), 7);
        EXPECT_EQ(sv.symbol, "X");
        ++count;
    };
    Responder responder(callback, requestKeyInt(7));
    for (int i = 0; i < 3; ++i) {
        responder.onValue({"X", 10});
    }
    EXPECT_EQ(count, 3);
}

TEST(ResponderTest, ExceptionInCallbackIsCaught) {
    auto callback = [&](const RequestKey&, const StreamValue&) {
        throw std::runtime_error("callback error");
    };
    Responder responder(callback, requestKeyInt(1));
    // onValue should catch exceptions and not propagate
    EXPECT_NO_THROW(responder.onValue({"S", 0}));
}

TEST(ResponderTest, ShutdownStopsSending) {
    int count = 0;
    auto callback = [&](const RequestKey&, const StreamValue&) { ++count; };
    Responder responder(callback, requestKeyInt(100));
    responder.onValue({"S", 5});
    EXPECT_EQ(count, 1);
    responder.shutdown();
    // After shutdown, callback is cleared — onValue is a no-op
    responder.onValue({"S", 10});
    EXPECT_EQ(count, 1);
}

TEST(ResponderTest, NullCallbackSafe) {
    Responder responder(nullptr, requestKeyInt(0));
    // Should not throw bad_function_call
    EXPECT_NO_THROW(responder.onValue({"S", 1}));
}

// Phase 1 of gma-string-id-subscriptions: the string-id path through
// Responder. Callback receives a RequestKey whose alternative is the
// std::string. Phase 2 adds round-trip integration tests through
// ClientSession.
TEST(ResponderTest, StringKeyIsThreadedThroughCallback) {
    RequestKey capturedKey;
    auto callback = [&](const RequestKey& key, const StreamValue&) {
        capturedKey = key;
    };
    Responder responder(callback, requestKeyStr("r-NEXO-open"));
    responder.onValue({"NEXO", 100.5});
    ASSERT_EQ(capturedKey.index(), 1u);
    EXPECT_EQ(std::get<std::string>(capturedKey), "r-NEXO-open");
}
