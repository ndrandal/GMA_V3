#include "gma/TreeBuilder.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/Dispatcher.hpp"
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
    std::vector<StreamValue> received;
    void onValue(const StreamValue& sv) override { received.push_back(sv); }
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
    std::unique_ptr<Dispatcher> dispatcher;
    tree::Deps deps;

    void initDeps() {
        dispatcher = std::make_unique<Dispatcher>(gThreadPool.get(), &store);
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
    doc.Parse(R"({"tree":{"type":"Listener","streamKey":"SYM","field":"price"}})");
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
    doc.Parse(R"({"id":"1","streamKey":"SYM","field":"price"})");
    ASSERT_FALSE(doc.HasParseError());

    auto chain = tree::buildForRequest(doc, deps, terminal);
    EXPECT_NE(chain.head, nullptr);
}

// ENC-101 push-vs-pull rule (see GMA_V3/docs/atomic-keys.md). A request
// asking for a Listener-on-`ob.*` must surface as a runtime_error
// during buildForRequest, not silently produce a Listener that never
// fires. ClientSession's subscribe/validate `try { TreeBuilder } catch`
// chain (src/server/ClientSession.cpp:456) propagates this to the WS
// peer as `{"type":"error","where":"validate", ...}`.
TEST_F(TreeBuilderTestFixture, BuildForRequestRejectsObListenerField) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"id":"1","streamKey":"NEXO","field":"ob.best.bid.price"})");
    ASSERT_FALSE(doc.HasParseError());

    try {
        (void)tree::buildForRequest(doc, deps, terminal);
        FAIL() << "buildForRequest should have thrown for ob.* listener";
    } catch (const std::runtime_error& ex) {
        const std::string what = ex.what();
        EXPECT_NE(what.find("pipeline-only"), std::string::npos)
            << "thrown message must contain 'pipeline-only'; got: " << what;
        EXPECT_NE(what.find("ob.best.bid.price"), std::string::npos)
            << "thrown message must echo the offending field; got: " << what;
    }
}

TEST_F(TreeBuilderTestFixture, BuildForRequestRejectsObSpread) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"id":"1","streamKey":"NEXO","field":"ob.spread"})");
    ASSERT_FALSE(doc.HasParseError());

    EXPECT_THROW(tree::buildForRequest(doc, deps, terminal), std::runtime_error);
}
