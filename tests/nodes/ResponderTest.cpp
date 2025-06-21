#include "gma/nodes/Responder.hpp"
#include "gma/SymbolValue.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <variant>

using namespace gma;
using namespace gma::nodes;

TEST(ResponderTest, CallsCallbackWithCorrectKeyAndValue) {
    int capturedKey = 0;
    SymbolValue capturedSv;
    auto callback = [&](int key, const SymbolValue& sv) {
        capturedKey = key;
        capturedSv = sv;
    };
    Responder responder(callback, 42);
    SymbolValue sv{"ABC", 3.14};
    responder.onValue(sv);
    EXPECT_EQ(capturedKey, 42);
    EXPECT_EQ(capturedSv.symbol, "ABC");
    EXPECT_DOUBLE_EQ(std::get<double>(capturedSv.value), 3.14);
}

TEST(ResponderTest, MultipleInvocations) {
    int count = 0;
    auto callback = [&](int key, const SymbolValue& sv) {
        EXPECT_EQ(key, 7);
        EXPECT_EQ(sv.symbol, "X");
        ++count;
    };
    Responder responder(callback, 7);
    for (int i = 0; i < 3; ++i) {
        responder.onValue({"X", 10});
    }
    EXPECT_EQ(count, 3);
}

TEST(ResponderTest, ExceptionInCallbackIsCaught) {
    auto callback = [&](int, const SymbolValue&) {
        throw std::runtime_error("callback error");
    };
    Responder responder(callback, 1);
    // onValue should catch exceptions and not propagate
    EXPECT_NO_THROW(responder.onValue({"S", 0}));
}

TEST(ResponderTest, ShutdownDoesNotAffectOnValue) {
    bool called = false;
    auto callback = [&](int, const SymbolValue&) { called = true; };
    Responder responder(callback, 100);
    responder.shutdown();
    // shutdown is a no-op; subsequent onValue should still invoke callback
    responder.onValue({"S", 5});
    EXPECT_TRUE(called);
}
