// End-to-end test for ENC-30 (wire ConfigNamespaceRegistry into
// Config::loadFromFile): a previously-unknown key like "binance.apiKey"
// must reach a connector-registered ConfigReaderFn through the deferred
// dispatchPendingKeys() flush, without any change to engine code.

#include "gma/engine/ConfigNamespaceRegistry.hpp"
#include "gma/util/Config.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::string writeTempIni(const std::string& body) {
  std::string path = std::tmpnam(nullptr);
  std::ofstream f(path);
  f << body;
  f.close();
  return path;
}

} // namespace

TEST(ConfigNamespaceDispatchTest, UnknownKeyRoutesToRegisteredNamespace) {
  // Use a unique prefix so this test doesn't collide with any
  // production reader the test bootstrap may have registered.
  const std::string prefix = "enc30test1";

  std::string sawTail;
  std::string sawValue;
  std::atomic<int> calls{0};

  gma::engine::ConfigNamespaceRegistry::registerNamespace(prefix,
    [&](std::string_view tail, std::string_view value) {
      sawTail.assign(tail);
      sawValue.assign(value);
      calls.fetch_add(1);
      return true;
    });

  auto path = writeTempIni(prefix + ".apiKey = supersecret\n");

  gma::util::Config cfg;
  ASSERT_TRUE(cfg.loadFromFile(path));
  std::remove(path.c_str());

  // Reader is NOT invoked during loadFromFile — keys are parked.
  EXPECT_EQ(calls.load(), 0);

  std::size_t consumed = cfg.dispatchPendingKeys();
  EXPECT_EQ(consumed, 1u);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(sawTail, "apiKey");
  EXPECT_EQ(sawValue, "supersecret");
}

TEST(ConfigNamespaceDispatchTest, FlatKeysWithoutDotAreNotDispatched) {
  // ConfigNamespaceRegistry only handles dotted keys (prefix.tail). A flat
  // key parked by an unknown engine name stays parked but is dropped on
  // flush — and never reaches a reader.
  const std::string prefix = "enc30test2";
  std::atomic<int> calls{0};
  gma::engine::ConfigNamespaceRegistry::registerNamespace(prefix,
    [&](std::string_view, std::string_view) { calls.fetch_add(1); return true; });

  auto path = writeTempIni("flatThing = whatever\n");
  gma::util::Config cfg;
  ASSERT_TRUE(cfg.loadFromFile(path));
  std::remove(path.c_str());

  std::size_t consumed = cfg.dispatchPendingKeys();
  EXPECT_EQ(consumed, 0u);
  EXPECT_EQ(calls.load(), 0);
}

TEST(ConfigNamespaceDispatchTest, EngineKnownKeysDoNotPark) {
  // Keys the engine recognizes (like wsPort) are parsed inline and never
  // reach the namespace registry, even if a wildcard-ish reader is
  // registered. This guards against accidental double-handling.
  const std::string prefix = "wsPort";  // intentionally clashing
  std::atomic<int> calls{0};
  gma::engine::ConfigNamespaceRegistry::registerNamespace(prefix,
    [&](std::string_view, std::string_view) { calls.fetch_add(1); return true; });

  auto path = writeTempIni("wsPort = 9876\n");
  gma::util::Config cfg;
  ASSERT_TRUE(cfg.loadFromFile(path));
  std::remove(path.c_str());

  EXPECT_EQ(cfg.wsPort, 9876);
  std::size_t consumed = cfg.dispatchPendingKeys();
  EXPECT_EQ(consumed, 0u);
  EXPECT_EQ(calls.load(), 0);
}

TEST(ConfigNamespaceDispatchTest, FlushIsIdempotent) {
  // Calling dispatchPendingKeys twice consumes once — the second call has
  // an empty parking list.
  const std::string prefix = "enc30test4";
  std::atomic<int> calls{0};
  gma::engine::ConfigNamespaceRegistry::registerNamespace(prefix,
    [&](std::string_view, std::string_view) { calls.fetch_add(1); return true; });

  auto path = writeTempIni(prefix + ".x = 1\n" + prefix + ".y = 2\n");
  gma::util::Config cfg;
  ASSERT_TRUE(cfg.loadFromFile(path));
  std::remove(path.c_str());

  EXPECT_EQ(cfg.dispatchPendingKeys(), 2u);
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(cfg.dispatchPendingKeys(), 0u);
  EXPECT_EQ(calls.load(), 2);
}
