#include "gma/AtomicStore.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <string>
#include <vector>
#include <atomic>

using namespace gma;

// Helper to retrieve a value and assert its presence
template<typename T>
static T getValue(const AtomicStore& store, const std::string& sym, const std::string& field) {
    auto val = store.get(sym, field);
    EXPECT_TRUE(val.has_value()) << "Expected value for " << sym << "::" << field;
    if (!val.has_value()) {
        return T{};
    }
    return std::get<T>(*val);
}

TEST(AtomicStoreTest, SetAndGetInteger) {
    AtomicStore store;
    store.set("SYM", "intField", 42);
    EXPECT_EQ(getValue<int>(store, "SYM", "intField"), 42);
}

TEST(AtomicStoreTest, SetAndGetDouble) {
    AtomicStore store;
    store.set("SYM", "dblField", 3.1415);
    EXPECT_DOUBLE_EQ(getValue<double>(store, "SYM", "dblField"), 3.1415);
}

TEST(AtomicStoreTest, SetAndGetBool) {
    AtomicStore store;
    store.set("SYM", "boolField", true);
    EXPECT_TRUE(getValue<bool>(store, "SYM", "boolField"));
}

TEST(AtomicStoreTest, SetAndGetString) {
    AtomicStore store;
    std::string s = "hello";
    store.set("SYM", "strField", s);
    EXPECT_EQ(getValue<std::string>(store, "SYM", "strField"), s);
}

TEST(AtomicStoreTest, SetAndGetVectorInt) {
    AtomicStore store;
    std::vector<int> v = {1, 2, 3};
    store.set("SYM", "vecInt", v);
    EXPECT_EQ(getValue<std::vector<int>>(store, "SYM", "vecInt"), v);
}

TEST(AtomicStoreTest, SetAndGetVectorDouble) {
    AtomicStore store;
    std::vector<double> v = {1.1, 2.2, 3.3};
    store.set("SYM", "vecDbl", v);
    EXPECT_EQ(getValue<std::vector<double>>(store, "SYM", "vecDbl"), v);
}

TEST(AtomicStoreTest, MultipleFieldsUnderSameSymbol) {
    AtomicStore store;
    store.set("SYM", "a", 1);
    store.set("SYM", "b", 2);
    EXPECT_EQ(getValue<int>(store, "SYM", "a"), 1);
    EXPECT_EQ(getValue<int>(store, "SYM", "b"), 2);
}

TEST(AtomicStoreTest, OverwriteFieldUpdatesValue) {
    AtomicStore store;
    store.set("SYM", "field", 10);
    store.set("SYM", "field", 20);
    EXPECT_EQ(getValue<int>(store, "SYM", "field"), 20);
}

TEST(AtomicStoreTest, SeparateSymbolsIsolation) {
    AtomicStore store;
    store.set("SYM1", "f", 5);
    store.set("SYM2", "f", 10);
    EXPECT_EQ(getValue<int>(store, "SYM1", "f"), 5);
    EXPECT_EQ(getValue<int>(store, "SYM2", "f"), 10);
}

TEST(AtomicStoreTest, GetNonExistentSymbolOrField) {
    AtomicStore store;
    auto v1 = store.get("NOPE", "x");
    EXPECT_FALSE(v1.has_value());
    store.set("SYM", "a", 1);
    auto v2 = store.get("SYM", "b");
    EXPECT_FALSE(v2.has_value());
}

TEST(AtomicStoreTest, ConcurrentSetSameKey) {
    AtomicStore store;
    const int threads = 5;
    std::vector<std::thread> ths;
    for (int i = 0; i < threads; ++i) {
        ths.emplace_back([&store, i]() {
            store.set("CON", "k", i);
        });
    }
    for (auto& t : ths) t.join();
    auto v = store.get("CON", "k");
    ASSERT_TRUE(v.has_value());
    int val = std::get<int>(*v);
    EXPECT_GE(val, 0);
    EXPECT_LT(val, threads);
}

TEST(AtomicStoreTest, ConcurrentGetDuringSet) {
    AtomicStore store;
    std::atomic<bool> done{false};
    std::thread writer([&]() {
        for (int i = 0; i < 1000; ++i) {
            store.set("TST", "val", i);
        }
        done = true;
    });
    std::thread reader([&]() {
        while (!done) {
            store.get("TST", "val");
        }
    });
    writer.join();
    reader.join();
    SUCCEED();
}
