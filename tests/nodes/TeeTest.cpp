#include "gma/nodes/Tee.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace gma;

namespace {

// Sink stub: records every received value and whether shutdown() was called.
class TestSink : public INode {
public:
  std::mutex mx;
  std::vector<StreamValue> received;
  std::atomic<int> count{0};
  std::atomic<bool> shutdownCalled{false};

  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx);
    received.push_back(sv);
    ++count;
  }
  void shutdown() noexcept override { shutdownCalled.store(true); }
};

double asDouble(const ArgType& a) { return std::get<double>(a); }

std::shared_ptr<Tee> makeTee(std::vector<std::shared_ptr<TestSink>>& sinks,
                             std::size_t n) {
  std::vector<std::shared_ptr<INode>> outs;
  for (std::size_t i = 0; i < n; ++i) {
    auto s = std::make_shared<TestSink>();
    sinks.push_back(s);
    outs.push_back(s);
  }
  return std::make_shared<Tee>(std::move(outs));
}

} // namespace

TEST(TeeTest, ForwardsOneValueToAllOutputs) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 3);

  tee->onValue(StreamValue{"SYM", 42.0});

  for (auto& s : sinks) {
    ASSERT_EQ(s->count.load(), 1);
    EXPECT_EQ(s->received[0].symbol, "SYM");
    EXPECT_DOUBLE_EQ(asDouble(s->received[0].value), 42.0);
  }
}

TEST(TeeTest, PreservesValueAndSymbolUnchanged) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 2);

  tee->onValue(StreamValue{"AAPL", 1.5});
  tee->onValue(StreamValue{"MSFT", -7.0});

  for (auto& s : sinks) {
    ASSERT_EQ(s->count.load(), 2);
    EXPECT_EQ(s->received[0].symbol, "AAPL");
    EXPECT_DOUBLE_EQ(asDouble(s->received[0].value), 1.5);
    EXPECT_EQ(s->received[1].symbol, "MSFT");
    EXPECT_DOUBLE_EQ(asDouble(s->received[1].value), -7.0);
  }
}

TEST(TeeTest, ForwardsEveryValueInOrder) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 2);

  std::vector<double> inputs{10.0, 20.0, 30.0, 40.0};
  for (double v : inputs) tee->onValue(StreamValue{"X", v});

  for (auto& s : sinks) {
    ASSERT_EQ(s->count.load(), static_cast<int>(inputs.size()));
    for (std::size_t i = 0; i < inputs.size(); ++i)
      EXPECT_DOUBLE_EQ(asDouble(s->received[i].value), inputs[i]);
  }
}

TEST(TeeTest, SingleOutputBehavesAsPassThrough) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 1);

  tee->onValue(StreamValue{"S", 3.14});
  ASSERT_EQ(sinks[0]->count.load(), 1);
  EXPECT_DOUBLE_EQ(asDouble(sinks[0]->received[0].value), 3.14);
}

TEST(TeeTest, ShutdownPropagatesToAllOutputs) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 3);

  EXPECT_EQ(tee->size(), 3u);
  tee->shutdown();

  for (auto& s : sinks) EXPECT_TRUE(s->shutdownCalled.load());
  EXPECT_EQ(tee->size(), 0u);
}

TEST(TeeTest, DropsValuesAfterShutdown) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 2);

  tee->onValue(StreamValue{"S", 1.0});
  tee->shutdown();
  tee->onValue(StreamValue{"S", 2.0}); // must be dropped

  for (auto& s : sinks) EXPECT_EQ(s->count.load(), 1);
}

TEST(TeeTest, DoubleShutdownIsSafe) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 2);

  tee->shutdown();
  tee->shutdown(); // idempotent — must not crash or rethrow
  for (auto& s : sinks) EXPECT_TRUE(s->shutdownCalled.load());
}

TEST(TeeTest, ConcurrentOnValueForwardsAll) {
  std::vector<std::shared_ptr<TestSink>> sinks;
  auto tee = makeTee(sinks, 2);

  constexpr int kThreads = 4;
  constexpr int kPerThread = 250;
  std::vector<std::thread> ts;
  for (int t = 0; t < kThreads; ++t) {
    ts.emplace_back([&tee, t] {
      for (int i = 0; i < kPerThread; ++i)
        tee->onValue(StreamValue{"S", static_cast<double>(t * 1000 + i)});
    });
  }
  for (auto& th : ts) th.join();

  for (auto& s : sinks)
    EXPECT_EQ(s->count.load(), kThreads * kPerThread);
}
