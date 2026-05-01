// Pins the IConnector lifecycle contract documented on IConnector.hpp:
//   1. registerWith() exactly once
//   2. start() exactly once, after registerWith
//   3. stop() noexcept, after start; safe to call multiple times
//   4. multiple connectors stopped in reverse-registration order
//   5. double-start is a programmer error → throws std::logic_error
//
// Uses a hand-rolled MockConnector that records call ordering rather than
// touching any of the production connectors (their behavior is covered by
// MarketConnector / SyntheticConnector tests respectively).

#include "gma/engine/IConnector.hpp"
#include "gma/engine/Registries.hpp"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Mock that appends a tagged string to a shared trace whenever a lifecycle
// method is called. start() can optionally throw on the second call to
// exercise case 5.
class MockConnector final : public gma::engine::IConnector {
public:
  MockConnector(std::string tag, std::vector<std::string>* trace)
    : _tag(std::move(tag)), _trace(trace) {}

  std::string_view name() const override { return _tag; }

  void registerWith(gma::engine::EngineRegistries&) override {
    ++_registerCalls;
    _trace->push_back(_tag + ":registerWith");
  }

  void start() override {
    if (_started) {
      throw std::logic_error(_tag + ": double-start");
    }
    _started = true;
    _trace->push_back(_tag + ":start");
  }

  void stop() noexcept override {
    // Idempotent: multiple stop() calls are safe and only the first records.
    if (!_started) return;
    _started = false;
    _trace->push_back(_tag + ":stop");
  }

  int registerCalls() const { return _registerCalls; }

private:
  std::string _tag;
  std::vector<std::string>* _trace;
  bool _started{false};
  int  _registerCalls{0};
};

} // namespace

TEST(ConnectorLifecycleTest, RegisterWithCalledExactlyOnce) {
  std::vector<std::string> trace;
  MockConnector m("A", &trace);
  gma::engine::EngineRegistries regs{};
  m.registerWith(regs);
  EXPECT_EQ(m.registerCalls(), 1);
  ASSERT_EQ(trace.size(), 1u);
  EXPECT_EQ(trace[0], "A:registerWith");
}

TEST(ConnectorLifecycleTest, StartFollowsRegisterWith) {
  std::vector<std::string> trace;
  MockConnector m("A", &trace);
  gma::engine::EngineRegistries regs{};
  m.registerWith(regs);
  m.start();
  ASSERT_EQ(trace.size(), 2u);
  EXPECT_EQ(trace[0], "A:registerWith");
  EXPECT_EQ(trace[1], "A:start");
}

TEST(ConnectorLifecycleTest, StopOrderIsReverseOfRegistration) {
  std::vector<std::string> trace;
  MockConnector a("A", &trace);
  MockConnector b("B", &trace);
  gma::engine::EngineRegistries regs{};

  std::vector<gma::engine::IConnector*> connectors{&a, &b};
  for (auto* c : connectors) c->registerWith(regs);
  for (auto* c : connectors) c->start();
  // Reverse-order stop, mirroring main.cpp's "connectors-stop" step.
  for (auto it = connectors.rbegin(); it != connectors.rend(); ++it) (*it)->stop();

  ASSERT_EQ(trace.size(), 6u);
  EXPECT_EQ(trace[0], "A:registerWith");
  EXPECT_EQ(trace[1], "B:registerWith");
  EXPECT_EQ(trace[2], "A:start");
  EXPECT_EQ(trace[3], "B:start");
  // The critical assertion: B stops before A.
  EXPECT_EQ(trace[4], "B:stop");
  EXPECT_EQ(trace[5], "A:stop");
}

TEST(ConnectorLifecycleTest, StopIsIdempotent) {
  std::vector<std::string> trace;
  MockConnector m("A", &trace);
  gma::engine::EngineRegistries regs{};
  m.registerWith(regs);
  m.start();
  m.stop();
  m.stop();   // second call must not throw and must not record.
  m.stop();   // third call ditto.

  ASSERT_EQ(trace.size(), 3u);
  EXPECT_EQ(trace[2], "A:stop");
}

TEST(ConnectorLifecycleTest, DoubleStartThrowsLogicError) {
  std::vector<std::string> trace;
  MockConnector m("A", &trace);
  gma::engine::EngineRegistries regs{};
  m.registerWith(regs);
  m.start();
  EXPECT_THROW(m.start(), std::logic_error);
}
