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

// ----- Tee fan-out node (ENC-646) -----

TEST_F(TreeBuilderTestFixture, BuildsTeeFanoutFromJson) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({
      "type":"Tee",
      "outputs":[
        {"type":"Worker","fn":"last"},
        {"type":"Worker","fn":"last"}
      ]
    })");
    auto tee = tree::buildNode(doc, "SYM", deps, terminal);
    ASSERT_TRUE(tee);

    // One value into the Tee fans out through both Worker branches, each of
    // which forwards into the shared terminal — so the terminal sees it twice.
    tee->onValue(StreamValue{"SYM", 5.0});
    EXPECT_EQ(terminal->received.size(), 2u);
    for (const auto& sv : terminal->received)
        EXPECT_DOUBLE_EQ(std::get<double>(sv.value), 5.0);
}

TEST_F(TreeBuilderTestFixture, TeeRejectsMissingOutputs) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Tee"})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, TeeRejectsEmptyOutputs) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Tee","outputs":[]})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

// ----- Pack / Field record nodes (ENC-643) -----

TEST_F(TreeBuilderTestFixture, BuildsFieldFromJsonAndExtracts) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Field","name":"h"})");
    auto field = tree::buildNode(doc, "SYM", deps, terminal);
    ASSERT_TRUE(field);

    Record r;
    recordSet(r, "o", 1.0);
    recordSet(r, "h", 3.0);
    field->onValue(StreamValue{"SYM", ArgType{r}});

    ASSERT_EQ(terminal->received.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 3.0);
}

TEST_F(TreeBuilderTestFixture, FieldRejectsMissingName) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Field"})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, BuildsPackFromJson) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({
      "type":"Pack",
      "fields":{
        "a":{"type":"AtomicAccessor","field":"bid"},
        "b":{"type":"AtomicAccessor","field":"ask"}
      }
    })");
    auto pack = tree::buildNode(doc, "SYM", deps, terminal);
    EXPECT_NE(pack, nullptr);
}

TEST_F(TreeBuilderTestFixture, PackRejectsMissingFields) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Pack"})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, PackRejectsEmptyFields) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Pack","fields":{}})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

// ----- Expr node (ENC-645) -----

TEST_F(TreeBuilderTestFixture, BuildsExprFromJsonAndEvaluates) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Expr","expr":{"op":"add","args":[{"ref":"value"},100]}})");
    auto node = tree::buildNode(doc, "SYM", deps, terminal);
    ASSERT_TRUE(node);

    node->onValue(StreamValue{"SYM", 5.0});
    ASSERT_EQ(terminal->received.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 105.0);
}

TEST_F(TreeBuilderTestFixture, ExprRejectsMissingExpr) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Expr"})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, ExprRejectsMalformedExpr) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Expr","expr":{"op":"bogus","args":[1]}})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

// ----- Filter node (ENC-649) -----

TEST_F(TreeBuilderTestFixture, BuildsFilterFromJsonAndGates) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Filter","when":{"op":"gt","args":[{"ref":"value"},10]}})");
    auto node = tree::buildNode(doc, "SYM", deps, terminal);
    ASSERT_TRUE(node);

    node->onValue(StreamValue{"SYM", 5.0});   // dropped
    node->onValue(StreamValue{"SYM", 20.0});  // passed
    ASSERT_EQ(terminal->received.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 20.0);
}

TEST_F(TreeBuilderTestFixture, FilterRejectsMissingWhen) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Filter"})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

// ----- Switch node (ENC-650) -----

TEST_F(TreeBuilderTestFixture, BuildsSwitchFromJsonAndRoutes) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    // select by value; two Worker(last) cases both forward into the terminal.
    doc.Parse(R"({
      "type":"Switch",
      "select":{"ref":"value"},
      "cases":[{"type":"Worker","fn":"last"},{"type":"Worker","fn":"last"}]
    })");
    auto node = tree::buildNode(doc, "SYM", deps, terminal);
    ASSERT_TRUE(node);

    node->onValue(StreamValue{"SYM", 1.0});  // routes to case 1 -> terminal
    node->onValue(StreamValue{"SYM", 9.0});  // out of range, no default -> drop
    ASSERT_EQ(terminal->received.size(), 1u);
    EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 1.0);
}

TEST_F(TreeBuilderTestFixture, SwitchRejectsMissingSelect) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Switch","cases":[{"type":"Worker","fn":"last"}]})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST_F(TreeBuilderTestFixture, SwitchRejectsEmptyCases) {
    initDeps();
    auto terminal = std::make_shared<TerminalStub>();
    rapidjson::Document doc;
    doc.Parse(R"({"type":"Switch","select":{"ref":"value"},"cases":[]})");
    EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
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
