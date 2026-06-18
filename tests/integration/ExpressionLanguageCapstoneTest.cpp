// Capstone (ENC-651): the expression-language vocabulary composing end-to-end
// through the real engine — record (Pack/Field), Expr, Filter, and let-bindings
// together. Proves the pieces interoperate, not just in isolation.
#include "gma/TreeBuilder.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

using namespace gma;

namespace {

class Terminal : public INode {
public:
  std::mutex mx;
  std::vector<StreamValue> received;
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx);
    received.push_back(sv);
  }
  void shutdown() noexcept override {}
  std::size_t size() {
    std::lock_guard<std::mutex> lk(mx);
    return received.size();
  }
};

struct Rig {
  rt::ThreadPool pool{1};
  AtomicStore store;
  Dispatcher dispatcher{&pool, &store};
  std::shared_ptr<Terminal> terminal = std::make_shared<Terminal>();
  tree::Deps deps;
  Rig() {
    deps.store = &store;
    deps.pool = &pool;
    deps.dispatcher = &dispatcher;
  }
  // Build a downstream-first chain of node specs: chain[last] -> terminal, ...,
  // chain[0] is returned as the head to drive.
  std::shared_ptr<INode> buildChain(std::initializer_list<const char*> specs) {
    std::vector<const char*> v(specs);
    std::shared_ptr<INode> down = terminal;
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
      rapidjson::Document d;
      d.Parse(*it);
      EXPECT_FALSE(d.HasParseError()) << *it;
      down = tree::buildNode(d, "SYM", deps, down);
    }
    return down;
  }
  double at(std::size_t i) { return std::get<double>(terminal->received[i].value); }
};

} // namespace

// "Emit the price whenever RSI > 70" — Pack(rsi,price) -> Filter(rsi>70) ->
// Field(price). Record + Filter + Field composed end-to-end.
TEST(ExpressionLanguageCapstoneTest, EmitPriceWhenRsiAboveThreshold) {
  Rig rig;
  auto head = rig.buildChain({
    R"({"type":"Pack","fields":{
        "rsi":{"type":"Listener","streamKey":"SYM","field":"rsi"},
        "price":{"type":"Listener","streamKey":"SYM","field":"price"}
    }})",
    R"({"type":"Filter","when":{"op":"gt","args":[{"ref":"rsi"},70]}})",
    R"({"type":"Field","name":"price"})"
  });
  ASSERT_TRUE(head);

  rig.dispatcher.notifyListeners("SYM", "price", 100.0);  // price latest = 100
  rig.dispatcher.notifyListeners("SYM", "rsi", 80.0);     // complete, 80>70  -> emit 100
  rig.dispatcher.notifyListeners("SYM", "rsi", 50.0);     // 50>70 false      -> drop
  rig.dispatcher.notifyListeners("SYM", "rsi", 90.0);     // 90>70            -> emit 100
  rig.pool.shutdown();

  ASSERT_EQ(rig.terminal->size(), 2u);
  EXPECT_DOUBLE_EQ(rig.at(0), 100.0);
  EXPECT_DOUBLE_EQ(rig.at(1), 100.0);
}

// Derived signal: Pack(bid,ask) -> Expr((bid+ask)/2) — record + expression.
TEST(ExpressionLanguageCapstoneTest, ExprComputesMidFromRecord) {
  Rig rig;
  auto head = rig.buildChain({
    R"({"type":"Pack","fields":{
        "bid":{"type":"Listener","streamKey":"SYM","field":"bid"},
        "ask":{"type":"Listener","streamKey":"SYM","field":"ask"}
    }})",
    R"({"type":"Expr","expr":{"op":"div","args":[
        {"op":"add","args":[{"ref":"bid"},{"ref":"ask"}]}, 2]}})"
  });
  ASSERT_TRUE(head);

  rig.dispatcher.notifyListeners("SYM", "bid", 10.0);
  rig.dispatcher.notifyListeners("SYM", "ask", 12.0);   // complete -> (10+12)/2 = 11
  rig.pool.shutdown();

  ASSERT_EQ(rig.terminal->size(), 1u);
  EXPECT_DOUBLE_EQ(rig.at(0), 11.0);
}

// Reuse: a let-binding feeds two derived expressions; the source is produced
// once and fanned to both. Each Aggregate input is Chain[Ref(p) -> Expr], so
// the single price event drives two distinct expressions over the shared value.
TEST(ExpressionLanguageCapstoneTest, LetBindingReusedByTwoExpressions) {
  Rig rig;
  rapidjson::Document doc;
  doc.Parse(R"({
    "type":"Let",
    "bindings":{ "p":{"type":"Listener","streamKey":"SYM","field":"price"} },
    "body":{
      "type":"Aggregate","arity":2,
      "inputs":[
        {"type":"Chain","stages":[
          {"type":"Ref","name":"p"},
          {"type":"Expr","expr":{"op":"mul","args":[{"ref":"value"},10]}}
        ]},
        {"type":"Chain","stages":[
          {"type":"Ref","name":"p"},
          {"type":"Expr","expr":{"op":"add","args":[{"ref":"value"},1]}}
        ]}
      ]
    }
  })");
  ASSERT_FALSE(doc.HasParseError());

  auto root = tree::buildNode(doc, "SYM", rig.deps, rig.terminal);
  ASSERT_TRUE(root);

  rig.dispatcher.notifyListeners("SYM", "price", 4.0);  // 4*10=40 and 4+1=5
  rig.pool.shutdown();

  // Aggregate(2) collects both derived values from the single source event.
  ASSERT_EQ(rig.terminal->size(), 2u);
  std::vector<double> got{rig.at(0), rig.at(1)};
  std::sort(got.begin(), got.end());
  EXPECT_DOUBLE_EQ(got[0], 5.0);    // value + 1
  EXPECT_DOUBLE_EQ(got[1], 40.0);   // value * 10
  root->shutdown();
}
