#include "gma/ExecutionContext.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <memory>

using namespace gma;

TEST(ExecutionContextTest, StoresPointers) {
    AtomicStore store;
    ThreadPool pool(2);
    ExecutionContext ctx(&store, &pool);

    EXPECT_EQ(ctx.store(), &store);
    EXPECT_EQ(ctx.pool(), &pool);
}

TEST(ExecutionContextTest, HandlesNullptr) {
    ExecutionContext ctx(nullptr, nullptr);

    EXPECT_EQ(ctx.store(), nullptr);
    EXPECT_EQ(ctx.pool(), nullptr);
}
