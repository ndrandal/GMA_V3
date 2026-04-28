#include "gma/TreeBuilder.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/Listener.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <fstream>
#include <memory>
#include <string>
#include <iostream>

using namespace gma;

// Stub terminal node
class CorpusTerminal : public INode {
public:
    void onValue(const StreamValue&) override {}
    void shutdown() noexcept override {}
};

class CorpusTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        gThreadPool = std::make_shared<rt::ThreadPool>(2);
        dispatcher = std::make_unique<Dispatcher>(gThreadPool.get(), &store);
        deps.store = &store;
        deps.pool = gThreadPool.get();
        deps.dispatcher = dispatcher.get();
    }

    void TearDown() override {
        if (dispatcher) dispatcher.reset();
        if (gThreadPool) {
            gThreadPool->shutdown();
            gThreadPool.reset();
        }
    }

    AtomicStore store;
    std::unique_ptr<Dispatcher> dispatcher;
    tree::Deps deps;
};

TEST_F(CorpusTestFixture, AllCorpusRequestsBuild) {
    // Try multiple search paths for the corpus file
    std::string paths[] = {
        "corpus_requests.json",
        "../tests/treebuilder/corpus_requests.json",
        "tests/treebuilder/corpus_requests.json",
    };

    std::ifstream ifs;
    std::string usedPath;
    for (const auto& p : paths) {
        ifs.open(p);
        if (ifs.is_open()) {
            usedPath = p;
            break;
        }
    }

    if (!ifs.is_open()) {
        GTEST_SKIP() << "corpus_requests.json not found — skipping corpus test";
        return;
    }

    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);
    ifs.close();

    ASSERT_FALSE(doc.HasParseError()) << "Failed to parse corpus JSON";
    ASSERT_TRUE(doc.IsArray()) << "Corpus must be a JSON array";

    int passed = 0;
    int failed = 0;
    int total = doc.Size();

    for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        const auto& entry = doc[i];
        int corpusId = entry.HasMember("corpus_id") ? entry["corpus_id"].GetInt() : -1;
        std::string nl = entry.HasMember("nl") ? entry["nl"].GetString() : "";

        ASSERT_TRUE(entry.HasMember("request")) << "Entry " << i << " missing 'request'";
        const auto& req = entry["request"];

        auto terminal = std::make_shared<CorpusTerminal>();

        try {
            auto chain = tree::buildForRequest(req, deps, terminal);
            EXPECT_NE(chain.head, nullptr)
                << "Corpus #" << corpusId << " built null head: " << nl;

            // Shut down to clean up Listener/Interval threads
            if (chain.head) chain.head->shutdown();
            for (auto& node : chain.keepAlive) {
                if (node) node->shutdown();
            }

            ++passed;
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Corpus #" << corpusId << " FAILED: " << e.what()
                          << "\n  NL: " << nl;
            ++failed;
        }
    }

    std::cout << "\n[CorpusTest] " << passed << "/" << total << " requests built successfully\n";
    if (failed > 0) {
        std::cout << "[CorpusTest] " << failed << " FAILURES\n";
    }
}
