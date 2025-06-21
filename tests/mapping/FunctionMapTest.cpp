#include "gma/FunctionMap.hpp"
#include <gtest/gtest.h>
#include <numeric>
#include <vector>
#include <thread>
#include <atomic>

using namespace gma;

TEST(FunctionMapTest, RegisterAndRetrieve) {
    auto& fm = FunctionMap::instance();
    // Register a sum function
    fm.registerFunction("sumTest", [](const std::vector<double>& v) {
        return std::accumulate(v.begin(), v.end(), 0.0);
    });
    auto sumFn = fm.getFunction("sumTest");
    std::vector<double> data{1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(sumFn(data), 6.0);
}

TEST(FunctionMapTest, OverwriteFunction) {
    auto& fm = FunctionMap::instance();
    // Initial registration returns 1.0
    fm.registerFunction("overwriteTest", [](const std::vector<double>&) {
        return 1.0;
    });
    auto fn1 = fm.getFunction("overwriteTest");
    EXPECT_DOUBLE_EQ(fn1({}), 1.0);
    // Overwrite to return 2.0
    fm.registerFunction("overwriteTest", [](const std::vector<double>&) {
        return 2.0;
    });
    auto fn2 = fm.getFunction("overwriteTest");
    EXPECT_DOUBLE_EQ(fn2({}), 2.0);
}

TEST(FunctionMapTest, GetAllContainsRegistered) {
    auto& fm = FunctionMap::instance();
    // Snapshot before registration
    auto before = fm.getAll();
    // Register a test function
    fm.registerFunction("allTest", [](const std::vector<double>&) {
        return 0.0;
    });
    auto all = fm.getAll();
    // Expect at least one new entry
    EXPECT_GE(all.size(), before.size() + 1);
    // Verify our function is present
    bool found = false;
    for (const auto& kv : all) {
        if (kv.first == "allTest") { found = true; break; }
    }
    EXPECT_TRUE(found) << "Function 'allTest' should be listed in getAll()";
}

TEST(FunctionMapTest, GetFunctionThrowsIfNotFound) {
    auto& fm = FunctionMap::instance();
    // Use a name unlikely to be registered
    EXPECT_THROW(fm.getFunction("NoSuchFunctionXYZ"), std::runtime_error);
}

TEST(FunctionMapTest, ConcurrentRegistrationAndRetrieval) {
    auto& fm = FunctionMap::instance();
    std::atomic<bool> start{false};
    // Thread to register
    std::thread registrar([&](){
        while (!start.load()) std::this_thread::yield();
        fm.registerFunction("concTest", [](const std::vector<double>&) {
            return 42.0;
        });
    });
    // Thread to retrieve
    std::thread retriever([&](){
        while (!start.load()) std::this_thread::yield();
        // Spin until the function is available
        while (true) {
            try {
                auto fn = fm.getFunction("concTest");
                EXPECT_DOUBLE_EQ(fn({}), 42.0);
                break;
            } catch (const std::runtime_error&) {}
        }
    });
    // Start both threads
    start = true;
    registrar.join();
    retriever.join();
}
