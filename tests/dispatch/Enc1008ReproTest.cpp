// TEMPORARY ENC-1008 repro against master (no config flag exists yet).
#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <memory>
#include <mutex>
#include <vector>
using namespace gma;
namespace {
class Rec : public INode {
public:
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override {}
};
Event mk(const std::string& s, const std::vector<std::pair<std::string,double>>& f) {
  auto d = std::make_shared<rapidjson::Document>(); d->SetObject();
  auto& a = d->GetAllocator();
  for (auto& [n,v] : f) d->AddMember(rapidjson::Value(n.c_str(),a), rapidjson::Value(v), a);
  return Event{s, std::move(d)};
}
double asD(const ArgType& v) {
  if (std::holds_alternative<double>(v)) return std::get<double>(v);
  if (std::holds_alternative<int>(v)) return (double)std::get<int>(v);
  return 0.0;
}
}
TEST(Enc1008Repro, TwoFieldsDrivingTheSameBuiltinBothSurvive) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher md(&pool, &store);
  md.registerListener("NS.SYM", "alpha", std::make_shared<Rec>());
  md.registerListener("NS.SYM", "beta",  std::make_shared<Rec>());
  md.registerListener("NS.SYM", "mean",  std::make_shared<Rec>());
  md.onTick(mk("NS.SYM", {{"alpha",10.0},{"beta",100.0}}));
  md.onTick(mk("NS.SYM", {{"alpha",20.0},{"beta",200.0}}));
  pool.shutdown();

  auto flat = store.get("NS.SYM", "mean");
  ADD_FAILURE() << "DIAGNOSTIC — flat key `mean` = "
                << (flat ? std::to_string(asD(*flat)) : std::string("<absent>"))
                << "; `alpha.mean` present=" << store.get("NS.SYM","alpha.mean").has_value()
                << "; `beta.mean` present="  << store.get("NS.SYM","beta.mean").has_value();

  auto alpha = store.get("NS.SYM", "alpha.mean");
  auto beta  = store.get("NS.SYM", "beta.mean");
  ASSERT_TRUE(alpha.has_value()) << "alpha's derived mean was lost (ENC-1008)";
  ASSERT_TRUE(beta.has_value())  << "beta's derived mean was lost (ENC-1008)";
  EXPECT_DOUBLE_EQ(asD(*alpha), 15.0);
  EXPECT_DOUBLE_EQ(asD(*beta), 150.0);
}
