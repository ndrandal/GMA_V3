#include "gma/util/Config.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>

using namespace gma::util;

TEST(ConfigTest, DefaultValues) {
    Config cfg;
    EXPECT_EQ(cfg.taMACD_fast, 12);
    EXPECT_EQ(cfg.taMACD_slow, 26);
    EXPECT_EQ(cfg.taBBands_n, 20);
    EXPECT_DOUBLE_EQ(cfg.taBBands_stdK, 2.0);
}

TEST(ConfigTest, LoadFromFile) {
    const char* path = "test_config.ini";
    {
        std::ofstream f(path);
        f << "taMACD_fast=8\n"
          << "taMACD_slow=21\n"
          << "taBBands_n=15\n"
          << "taBBands_stdK=1.5\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 8);
    EXPECT_EQ(cfg.taMACD_slow, 21);
    EXPECT_EQ(cfg.taBBands_n, 15);
    EXPECT_DOUBLE_EQ(cfg.taBBands_stdK, 1.5);
    std::remove(path);
}

TEST(ConfigTest, LoadIgnoresComments) {
    const char* path = "test_config_comments.ini";
    {
        std::ofstream f(path);
        f << "# This is a comment\n"
          << "; So is this\n"
          << "taMACD_fast=5\n"
          << "\n"
          << "taMACD_slow=30\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 5);
    EXPECT_EQ(cfg.taMACD_slow, 30);
    std::remove(path);
}

TEST(ConfigTest, LoadIgnoresUnknownKeys) {
    const char* path = "test_config_unknown.ini";
    {
        std::ofstream f(path);
        f << "unknownKey=999\n"
          << "taMACD_fast=7\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taMACD_fast, 7);
    // Unknown keys should be silently ignored; defaults preserved for unset keys
    EXPECT_EQ(cfg.taMACD_slow, 26);
    std::remove(path);
}

TEST(ConfigTest, SwapsSlowFastIfInverted) {
    const char* path = "test_config_swap.ini";
    {
        std::ofstream f(path);
        f << "taMACD_fast=30\n"
          << "taMACD_slow=10\n";
    }
    Config cfg;
    cfg.loadFromFile(path);
    EXPECT_LE(cfg.taMACD_fast, cfg.taMACD_slow);
    std::remove(path);
}

TEST(ConfigTest, ReturnsFalseForMissingFile) {
    Config cfg;
    EXPECT_FALSE(cfg.loadFromFile("nonexistent_file_12345.ini"));
}

TEST(ConfigTest, NewTAFieldsParsing) {
    const char* path = "test_config_ta.ini";
    {
        std::ofstream f(path);
        f << "taSMA=3,7,15\n"
          << "taEMA=5,10\n"
          << "taRSI=7\n"
          << "taATR=10\n"
          << "taMomentum=5\n"
          << "taMACD_signal=7\n"
          << "taVolAvg=10\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.taSMA, (std::vector<int>{3, 7, 15}));
    EXPECT_EQ(cfg.taEMA, (std::vector<int>{5, 10}));
    EXPECT_EQ(cfg.taRSI, 7);
    EXPECT_EQ(cfg.taATR, 10);
    EXPECT_EQ(cfg.taMomentum, 5);
    EXPECT_EQ(cfg.taMACD_signal, 7);
    EXPECT_EQ(cfg.taVolAvg, 10);
    std::remove(path);
}

TEST(ConfigTest, DefaultNewTAFields) {
    Config cfg;
    EXPECT_EQ(cfg.taSMA, (std::vector<int>{5, 20}));
    EXPECT_EQ(cfg.taEMA, (std::vector<int>{12, 26}));
    EXPECT_EQ(cfg.taRSI, 14);
    EXPECT_EQ(cfg.taATR, 14);
    EXPECT_EQ(cfg.taMomentum, 10);
    EXPECT_EQ(cfg.taMACD_signal, 9);
    EXPECT_EQ(cfg.taVolAvg, 20);
}

// ENC-1008 — the derived-atomic key shape is INI-switchable, and defaults off.
// The default matters more than the parse: the key it controls is a
// client-visible WS `field` string, so a build that silently came up namespaced
// would break existing AtomicAccessor bindings (see docs/atomic-keys.md).
TEST(ConfigTest, AtomicKeyNamespaceByFieldDefaultsOff) {
    EXPECT_FALSE(Config{}.atomicKeyNamespaceByField);
}

TEST(ConfigTest, AtomicKeyNamespaceByFieldParsesTruthyForms) {
    for (const char* v : {"true", "1", "yes"}) {
        const char* path = "test_config_atomic_ns.ini";
        {
            std::ofstream f(path);
            f << "atomicKeyNamespaceByField=" << v << "\n";
        }
        Config cfg;
        EXPECT_TRUE(cfg.loadFromFile(path));
        EXPECT_TRUE(cfg.atomicKeyNamespaceByField) << "value: " << v;
        std::remove(path);
    }
}

TEST(ConfigTest, AtomicKeyNamespaceByFieldStaysOffForAnythingElse) {
    for (const char* v : {"false", "0", "no", ""}) {
        const char* path = "test_config_atomic_ns_off.ini";
        {
            std::ofstream f(path);
            f << "atomicKeyNamespaceByField=" << v << "\n";
        }
        Config cfg;
        EXPECT_TRUE(cfg.loadFromFile(path));
        EXPECT_FALSE(cfg.atomicKeyNamespaceByField) << "value: '" << v << "'";
        std::remove(path);
    }
}

// ---------------------------------------------------------------------------
// ENC-1331 — `gma_server <ws> <conf> <feed>` must BIND <feed>.
//
// The connector binds `ingress[].params["port"]`, not `cfg.feedPort`. Synthesis
// used to run at the end of loadFromFile(), i.e. before the composition root
// applied argv[3], so the file's port was frozen into the params while the boot
// log printed the argument. The two disagreed and only the bind was true — which
// is why these assert the resolved params, never the logged intent.
// ---------------------------------------------------------------------------

TEST(ConfigTest, LoadFromFileDoesNotSynthesizeIngress) {
    const char* path = "test_config_enc1331_nosynth.ini";
    {
        std::ofstream f(path);
        f << "wsPort=8180\n"
          << "feedPort=9101\n";
    }
    Config cfg;
    EXPECT_TRUE(cfg.loadFromFile(path));
    EXPECT_EQ(cfg.feedPort, 9101);
    // Nothing is captured yet: the caller still has CLI overrides to apply.
    EXPECT_TRUE(cfg.ingress.empty())
        << "loadFromFile() must not synthesize ingress — it would freeze the "
           "file's feedPort into the params that bind the socket";
    std::remove(path);
}

TEST(ConfigTest, FeedPortOverrideAfterLoadReachesIngressParams) {
    const char* path = "test_config_enc1331_override.ini";
    {
        std::ofstream f(path);
        f << "wsPort=8180\n"
          << "feedPort=9101\n";
    }
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    std::remove(path);

    // What src/main.cpp does with argv[3], in the order it does it.
    cfg.feedPort = 19191;
    EXPECT_TRUE(cfg.synthesizeIngressFromLegacy());

    ASSERT_FALSE(cfg.ingress.empty());
    EXPECT_EQ(cfg.ingress[0].kind, "market.feedserver");
    ASSERT_EQ(cfg.ingress[0].params.count("port"), 1u);
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "19191")
        << "the ingress entry that binds the feed socket must carry the "
           "overridden port, not the config file's";
}

TEST(ConfigTest, SynthesizeIsANoOpAndReportsItWhenIngressIsExplicit) {
    const char* path = "test_config_enc1331_explicit.ini";
    {
        std::ofstream f(path);
        f << "feedPort=9101\n"
          << "ingress.0.kind=market.feedserver\n"
          << "ingress.0.port=9300\n";
    }
    Config cfg;
    ASSERT_TRUE(cfg.loadFromFile(path));
    std::remove(path);

    cfg.feedPort = 19191;  // as if argv[3] had been passed
    // Explicit ingress entries are more specific than the legacy feedPort key
    // and are left alone; false is how the composition root knows to warn that
    // the argument went nowhere.
    EXPECT_FALSE(cfg.synthesizeIngressFromLegacy());
    ASSERT_EQ(cfg.ingress.size(), 1u);
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "9300");
}

TEST(ConfigTest, SynthesizeWithoutAnyFileUsesCompiledFeedPort) {
    Config cfg;  // no loadFromFile at all — the `./gma_server` path
    EXPECT_TRUE(cfg.synthesizeIngressFromLegacy());
    ASSERT_EQ(cfg.ingress.size(), 1u);
    EXPECT_EQ(cfg.ingress[0].kind, "market.feedserver");
    EXPECT_EQ(cfg.ingress[0].params.at("port"), "9001");
}
