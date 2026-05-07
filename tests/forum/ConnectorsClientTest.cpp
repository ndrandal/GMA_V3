// Tests the JSON-to-ingress translation done by
// gma::forum::ConnectorsClient::parseConnectorsJson. Decoupled from
// the network — fed directly with the wire shape forum's
// /api/connectors returns (cf.
// forum/internal/http/routes/connectors.go ListConnectors), so any
// drift in that endpoint surfaces here.

#include "gma/forum/ConnectorsClient.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

using gma::forum::ConnectorsClient;
using Ingress = gma::util::Config::Ingress;

namespace {

const std::string kFeedSimRow = R"([{
  "id": "84ca9d62-e0eb-5818-ab2f-d0fee8530e60",
  "name": "feed-simulator",
  "authMethod": "websocket",
  "endpointUrl": "wss://feed-sim.v3m.xyz/feed",
  "protocol": "itch",
  "symbols": ["NEXO","VALT","RAYM","STRT","DRRB"],
  "status": "connected",
  "resources": [
    {"name": "Equity Ticks", "fields": []}
  ]
}])";

}  // namespace

TEST(ConnectorsClientTest, MapsSingleFeedSimRow) {
  auto ingresses = ConnectorsClient::parseConnectorsJson(kFeedSimRow);
  ASSERT_EQ(ingresses.size(), 1u);
  const auto& ing = ingresses[0];

  EXPECT_EQ(ing.kind, "market.wsclient");

  auto urlIt = ing.params.find("url");
  ASSERT_NE(urlIt, ing.params.end());
  EXPECT_EQ(urlIt->second, "wss://feed-sim.v3m.xyz/feed");

  auto adapterIt = ing.params.find("adapter");
  ASSERT_NE(adapterIt, ing.params.end());
  EXPECT_EQ(adapterIt->second, "itch");

  auto symbolsIt = ing.params.find("symbols");
  ASSERT_NE(symbolsIt, ing.params.end());
  EXPECT_EQ(symbolsIt->second, "NEXO,VALT,RAYM,STRT,DRRB");
}

TEST(ConnectorsClientTest, SkipsConnectorsWithUnknownProtocol) {
  const std::string body = R"([
    {"name": "feed-simulator", "endpointUrl": "wss://x/feed", "protocol": "itch", "symbols": ["A"]},
    {"name": "binance", "endpointUrl": "wss://stream.binance.com/ws", "protocol": "binance-ws", "symbols": ["BTC"]}
  ])";
  auto ingresses = ConnectorsClient::parseConnectorsJson(body);
  ASSERT_EQ(ingresses.size(), 1u);
  EXPECT_EQ(ingresses[0].params.at("url"), "wss://x/feed");
}

TEST(ConnectorsClientTest, SkipsConnectorsWithEmptyEndpoint) {
  const std::string body = R"([
    {"name": "stub", "endpointUrl": "", "protocol": "itch", "symbols": ["A"]}
  ])";
  auto ingresses = ConnectorsClient::parseConnectorsJson(body);
  EXPECT_EQ(ingresses.size(), 0u);
}

TEST(ConnectorsClientTest, EmptySymbolsArrayDefaultsToWildcard) {
  const std::string body = R"([
    {"name": "feed-simulator", "endpointUrl": "wss://x/feed", "protocol": "itch", "symbols": []}
  ])";
  auto ingresses = ConnectorsClient::parseConnectorsJson(body);
  ASSERT_EQ(ingresses.size(), 1u);
  EXPECT_EQ(ingresses[0].params.at("symbols"), "*");
}

TEST(ConnectorsClientTest, RejectsNonArrayPayload) {
  // forum returns {"error":"..."} on auth failures; not a list — must throw.
  EXPECT_THROW(
      ConnectorsClient::parseConnectorsJson(R"({"error":"unauthorized"})"),
      std::runtime_error);
}

TEST(ConnectorsClientTest, RejectsMalformedJson) {
  EXPECT_THROW(
      ConnectorsClient::parseConnectorsJson("not-json-at-all"),
      std::runtime_error);
}

TEST(ConnectorsClientTest, EmptyArrayProducesNoIngresses) {
  auto ingresses = ConnectorsClient::parseConnectorsJson("[]");
  EXPECT_EQ(ingresses.size(), 0u);
}
