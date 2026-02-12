#include "gma/TreeBuilder.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/Listener.hpp"
#include "gma/JsonValidator.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <memory>

using namespace gma;

// Stub terminal node to capture output
class TerminalStub : public INode {
public:
    std::vector<SymbolValue> received;
    void onValue(const SymbolValue& sv) override { received.push_back(sv); }
    void shutdown() noexcept override {}
};

class TreeBuilderTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        gThreadPool = std::make_shared<rt::ThreadPool>(2);
    }
    void TearDown() override {
        if (gThreadPool) {
            gThreadPool->shutdown();
            gThreadPool.reset();
        }
    }

    AtomicStore store;
    std::unique_ptr<MarketDispatcher> dispatcher;
    tree::Deps deps;

    void initDeps() {
        dispatcher = std::make_unique<MarketDispatcher>(gThreadPool.get(), &store);
        deps.store = &store;
        deps.pool = gThreadPool.get();
        deps.dispatcher = dispatcher.get();
    }
};

TEST_F(TreeBuilderTestFixture, BuildForRequestRejectsEmptyJson) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse("{}");
    EXPECT_THROW(tree::buildForRequest(doc, deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, BuildForRequestRejectsMissingId) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"tree":{"type":"Listener","symbol":"SYM","field":"price"}})");
    EXPECT_THROW(tree::buildForRequest(doc, deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, BuildForRequestRejectsMissingTree) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"id":"1"})");
    EXPECT_THROW(tree::buildForRequest(doc, deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, BuildForRequestBuildsListener) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"id":"1","tree":{"type":"Listener","symbol":"SYM","field":"price"}})");
    ASSERT_FALSE(doc.HasParseError());

    auto chain = tree::buildForRequest(doc, deps, terminal);
    EXPECT_NE(chain.head, nullptr);
}
