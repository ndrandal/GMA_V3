#include "gma/TreeBuilder.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/Config.hpp"
#include "gma/nodes/Listener.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <memory>

using namespace gma;
using gma::nodes::Listener;

// Helper to build and return node or catch exception
static std::shared_ptr<INode> buildNode(const std::string& jsonStr,
                                         ExecutionContext* ctx,
                                         MarketDispatcher* md,
                                         bool expectOk) {
    rapidjson::Document doc;
    doc.Parse(jsonStr.c_str());
    if (expectOk) {
        EXPECT_FALSE(doc.HasParseError()) << "JSON parse error: " << jsonStr;
        auto node = TreeBuilder::build(doc, ctx, md);
        EXPECT_NE(node, nullptr);
        return node;
    } else {
        EXPECT_FALSE(doc.HasParseError());
        EXPECT_THROW(TreeBuilder::build(doc, ctx, md), std::runtime_error);
        return nullptr;
    }
}

TEST(TreeBuilderTest, RejectsEmptyJson) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    buildNode("{}", &ctx, &md, false);
}

TEST(TreeBuilderTest, RejectsMissingId) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    // Missing 'id' field
    buildNode("{\"tree\":{\"type\":\"Listener\",\"symbol\":\"SYM\",\"field\":\"f\"}}",
              &ctx, &md, false);
}

TEST(TreeBuilderTest, RejectsMissingTree) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    // Missing 'tree' field
    buildNode("{\"id\":\"1\"}", &ctx, &md, false);
}

TEST(TreeBuilderTest, RejectsMissingType) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    // Tree without 'type'
    buildNode("{\"id\":\"1\",\"tree\":{}}", &ctx, &md, false);
}

TEST(TreeBuilderTest, RejectsMissingSymbolOrField) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    // Missing symbol
    buildNode("{\"id\":\"1\",\"tree\":{\"type\":\"Listener\",\"field\":\"f\"}}",
              &ctx, &md, false);
    // Missing field
    buildNode("{\"id\":\"1\",\"tree\":{\"type\":\"Listener\",\"symbol\":\"SYM\"}}",
              &ctx, &md, false);
}

TEST(TreeBuilderTest, RejectsUnknownType) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    // Unsupported type
    buildNode("{\"id\":\"1\",\"tree\":{\"type\":\"Foo\"}}", &ctx, &md, false);
}

TEST(TreeBuilderTest, BuildsListener) {
    AtomicStore store;
    ThreadPool pool(Config::ThreadPoolSize);
    ExecutionContext ctx(&store, &pool);
    MarketDispatcher md(&pool, &store);
    auto node = buildNode(
        "{\"id\":\"1\",\"tree\":{\"type\":\"Listener\",\"symbol\":\"SYM\",\"field\":\"price\"}}",
        &ctx, &md, true);
    // Should be a Listener instance
    auto raw = node.get();
    auto* listenerPtr = dynamic_cast<Listener*>(raw);
    EXPECT_NE(listenerPtr, nullptr);
    EXPECT_EQ(listenerPtr->_symbol, "SYM");
}
