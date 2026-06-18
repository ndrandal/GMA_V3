#include "gma/nodes/Field.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace gma;

namespace {

class Sink : public INode {
public:
  std::vector<StreamValue> received;
  void onValue(const StreamValue& sv) override { received.push_back(sv); }
  void shutdown() noexcept override {}
};

ArgType recordOf(std::initializer_list<std::pair<const char*, double>> kvs) {
  Record r;
  for (auto& kv : kvs) recordSet(r, kv.first, kv.second);
  return ArgType{r};
}

} // namespace

TEST(FieldTest, ExtractsNamedField) {
  auto sink = std::make_shared<Sink>();
  Field f("h", sink);

  f.onValue(StreamValue{"SYM", recordOf({{"o", 1.0}, {"h", 3.0}, {"l", 0.5}})});

  ASSERT_EQ(sink->received.size(), 1u);
  EXPECT_EQ(sink->received[0].symbol, "SYM");
  EXPECT_DOUBLE_EQ(std::get<double>(sink->received[0].value), 3.0);
}

TEST(FieldTest, DropsNonRecordInput) {
  auto sink = std::make_shared<Sink>();
  Field f("h", sink);
  f.onValue(StreamValue{"SYM", 42.0}); // scalar, not a record
  EXPECT_TRUE(sink->received.empty());
}

TEST(FieldTest, DropsWhenFieldAbsent) {
  auto sink = std::make_shared<Sink>();
  Field f("missing", sink);
  f.onValue(StreamValue{"SYM", recordOf({{"o", 1.0}})});
  EXPECT_TRUE(sink->received.empty());
}

TEST(FieldTest, DropsAfterShutdown) {
  auto sink = std::make_shared<Sink>();
  Field f("o", sink);
  f.shutdown();
  f.onValue(StreamValue{"SYM", recordOf({{"o", 1.0}})});
  EXPECT_TRUE(sink->received.empty());
}
