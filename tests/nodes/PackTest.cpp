#include "gma/nodes/Pack.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace gma;

namespace {

class Sink : public INode {
public:
  std::vector<StreamValue> received;
  void onValue(const StreamValue& sv) override { received.push_back(sv); }
  void shutdown() noexcept override {}
};

// Build a Pack with `names` fields wired to fresh ports; returns the ports so
// the test can drive each field independently.
std::shared_ptr<Pack> makePack(const std::vector<std::string>& names,
                               std::shared_ptr<INode> sink,
                               std::vector<std::shared_ptr<INode>>& ports) {
  auto pack = std::make_shared<Pack>(names, std::move(sink));
  for (std::size_t i = 0; i < names.size(); ++i) {
    auto p = std::make_shared<PackPort>(std::weak_ptr<Pack>(pack), i);
    pack->addPort(p);
    ports.push_back(p);
  }
  return pack;
}

double field(const StreamValue& sv, const char* key) {
  const Record& r = std::get<Record>(sv.value);
  const ArgType* f = recordFind(r, key);
  return std::get<double>(*f);
}

} // namespace

TEST(PackTest, EmitsOnlyWhenAllFieldsSeen) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  auto pack = makePack({"a", "b"}, sink, ports);

  ports[0]->onValue(StreamValue{"S", 1.0});
  EXPECT_TRUE(sink->received.empty());        // incomplete

  ports[1]->onValue(StreamValue{"S", 2.0});
  ASSERT_EQ(sink->received.size(), 1u);        // now complete
  EXPECT_EQ(sink->received[0].symbol, "S");
  EXPECT_DOUBLE_EQ(field(sink->received[0], "a"), 1.0);
  EXPECT_DOUBLE_EQ(field(sink->received[0], "b"), 2.0);
}

TEST(PackTest, ReEmitsLatestOnSubsequentUpdate) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  auto pack = makePack({"a", "b"}, sink, ports);

  ports[0]->onValue(StreamValue{"S", 1.0});
  ports[1]->onValue(StreamValue{"S", 2.0});   // emit {a:1,b:2}
  ports[0]->onValue(StreamValue{"S", 5.0});   // emit {a:5,b:2}

  ASSERT_EQ(sink->received.size(), 2u);
  EXPECT_DOUBLE_EQ(field(sink->received[1], "a"), 5.0);
  EXPECT_DOUBLE_EQ(field(sink->received[1], "b"), 2.0);
}

TEST(PackTest, FieldOrderFollowsDeclaration) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  auto pack = makePack({"o", "h", "l", "c"}, sink, ports);

  ports[3]->onValue(StreamValue{"S", 4.0});
  ports[1]->onValue(StreamValue{"S", 2.0});
  ports[0]->onValue(StreamValue{"S", 1.0});
  ports[2]->onValue(StreamValue{"S", 3.0});   // completes

  ASSERT_EQ(sink->received.size(), 1u);
  const Record& r = std::get<Record>(sink->received[0].value);
  ASSERT_EQ(r.fields.size(), 4u);
  EXPECT_EQ(r.fields[0].name, "o");
  EXPECT_EQ(r.fields[1].name, "h");
  EXPECT_EQ(r.fields[2].name, "l");
  EXPECT_EQ(r.fields[3].name, "c");
}

TEST(PackTest, SymbolsAreIndependent) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  auto pack = makePack({"a", "b"}, sink, ports);

  ports[0]->onValue(StreamValue{"X", 1.0});
  ports[0]->onValue(StreamValue{"Y", 9.0});
  EXPECT_TRUE(sink->received.empty());          // neither symbol complete

  ports[1]->onValue(StreamValue{"Y", 8.0});     // only Y completes
  ASSERT_EQ(sink->received.size(), 1u);
  EXPECT_EQ(sink->received[0].symbol, "Y");
  EXPECT_DOUBLE_EQ(field(sink->received[0], "a"), 9.0);
  EXPECT_DOUBLE_EQ(field(sink->received[0], "b"), 8.0);
}

TEST(PackTest, DropsAfterShutdown) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  auto pack = makePack({"a", "b"}, sink, ports);

  ports[0]->onValue(StreamValue{"S", 1.0});
  pack->shutdown();
  ports[1]->onValue(StreamValue{"S", 2.0});     // dropped — no emit
  EXPECT_TRUE(sink->received.empty());
}

TEST(PackTest, PortDoesNotKeepPackAlive) {
  auto sink = std::make_shared<Sink>();
  std::vector<std::shared_ptr<INode>> ports;
  std::weak_ptr<Pack> weak;
  {
    auto pack = makePack({"a"}, sink, ports);
    weak = pack;
    EXPECT_FALSE(weak.expired());
  }
  // Pack dropped; the port's weak_ptr must not have kept it alive.
  EXPECT_TRUE(weak.expired());
  // Calling a now-orphaned port is a safe no-op.
  ports[0]->onValue(StreamValue{"S", 1.0});
  EXPECT_TRUE(sink->received.empty());
}
