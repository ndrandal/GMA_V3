// ENC-31: pins the ingress factory dispatch contract:
//   - Factories register under named kinds.
//   - The per-instance IngressParams map reaches the factory.
//   - start() / stop() are driven once each per instance, in order.
//   - Multiple factories can register; lookup by kind picks the right one.

#include "gma/engine/EngineRegistries.hpp"
#include "gma/engine/IngressRegistry.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace gma::engine;

namespace {

// Records lifecycle events from every MockIngress instance into a shared
// vector so test cases can assert ordering across instances.
struct CallTrace {
  std::mutex mx;
  std::vector<std::string> events;
  void push(const std::string& e) {
    std::lock_guard<std::mutex> lk(mx);
    events.push_back(e);
  }
};

class MockIngress final : public IIngressSource {
public:
  MockIngress(std::string tag, CallTrace* trace, IngressParams params)
    : tag_(std::move(tag)), trace_(trace), params_(std::move(params)) {
    trace_->push(tag_ + ":construct");
  }
  ~MockIngress() override { trace_->push(tag_ + ":destruct"); }

  void start() override          { trace_->push(tag_ + ":start"); }
  void stop()  noexcept override { trace_->push(tag_ + ":stop");  }

  const IngressParams& params() const { return params_; }

private:
  std::string  tag_;
  CallTrace*   trace_;
  IngressParams params_;
};

// Tiny stand-in for the driver loop in main.cpp — keeps this test
// self-contained while exercising the same registry/lookup path.
struct FakeEntry { std::string kind; IngressParams params; };
std::vector<std::unique_ptr<IIngressSource>> driveIngress(
    EngineRegistries& regs,
    const std::vector<FakeEntry>& entries) {
  std::vector<std::unique_ptr<IIngressSource>> out;
  for (const auto& e : entries) {
    const auto* f = IngressRegistry::find(e.kind);
    if (!f) continue;
    if (auto src = (*f)(regs, e.params)) out.push_back(std::move(src));
  }
  for (auto& s : out) s->start();
  return out;
}

} // namespace

TEST(IngressFactoryDispatchTest, FactoryReceivesPerInstanceParams) {
  IngressRegistry::clear();
  CallTrace trace;
  IngressParams sawParams;
  IngressRegistry::registerIngress("__test_kind__",
    [&trace, &sawParams](EngineRegistries&,
                          const IngressParams& params) -> std::unique_ptr<IIngressSource> {
      sawParams = params;
      return std::make_unique<MockIngress>("A", &trace, params);
    });

  EngineRegistries regs{};
  std::vector<FakeEntry> entries{{"__test_kind__", {{"port","9001"},{"name","x"}}}};
  auto live = driveIngress(regs, entries);

  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(sawParams.at("port"), "9001");
  EXPECT_EQ(sawParams.at("name"), "x");

  for (auto& s : live) s->stop();
  live.clear();

  // Construct + start + stop + destruct, exactly once.
  EXPECT_EQ(trace.events,
            (std::vector<std::string>{"A:construct","A:start","A:stop","A:destruct"}));
}

TEST(IngressFactoryDispatchTest, MultipleEntriesStartInOrderStopReverse) {
  IngressRegistry::clear();
  CallTrace trace;
  IngressRegistry::registerIngress("kind.a",
    [&trace](EngineRegistries&, const IngressParams& p) -> std::unique_ptr<IIngressSource> {
      return std::make_unique<MockIngress>("A", &trace, p);
    });
  IngressRegistry::registerIngress("kind.b",
    [&trace](EngineRegistries&, const IngressParams& p) -> std::unique_ptr<IIngressSource> {
      return std::make_unique<MockIngress>("B", &trace, p);
    });

  EngineRegistries regs{};
  std::vector<FakeEntry> entries{{"kind.a", {}}, {"kind.b", {}}};
  auto live = driveIngress(regs, entries);
  ASSERT_EQ(live.size(), 2u);

  // Stop in reverse-registration order, mirroring main.cpp's ingress-stop.
  for (auto it = live.rbegin(); it != live.rend(); ++it) (*it)->stop();
  live.clear();

  // Critical assertion: A starts before B; B stops before A.
  // Vector::clear destroys elements front-to-back, so destruct order is
  // A then B even though stop ran in reverse. The contract under test is
  // start/stop ordering — destruction is incidental.
  EXPECT_EQ(trace.events,
            (std::vector<std::string>{
              "A:construct","B:construct","A:start","B:start",
              "B:stop","A:stop","A:destruct","B:destruct"}));
}

TEST(IngressFactoryDispatchTest, UnknownKindIsNoOp) {
  IngressRegistry::clear();
  EngineRegistries regs{};
  std::vector<FakeEntry> entries{{"never.registered", {}}};
  auto live = driveIngress(regs, entries);
  EXPECT_TRUE(live.empty());
}

TEST(IngressFactoryDispatchTest, DuplicateRegistrationRejected) {
  IngressRegistry::clear();
  auto factory = [](EngineRegistries&, const IngressParams&)
      -> std::unique_ptr<IIngressSource> { return nullptr; };
  EXPECT_TRUE(IngressRegistry::registerIngress("dup", factory));
  EXPECT_FALSE(IngressRegistry::registerIngress("dup", factory));
}
