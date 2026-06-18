// End-to-end tests for let-bindings + Ref (ENC-647): a named binding's producer
// is built ONCE and fanned (via Tee) to every Ref consumer — the reuse DAG.
#include "gma/TreeBuilder.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>

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

tree::Deps makeDeps(AtomicStore& store, rt::ThreadPool& pool, Dispatcher& d) {
  tree::Deps deps;
  deps.store = &store;
  deps.pool = &pool;
  deps.dispatcher = &d;
  return deps;
}

} // namespace

TEST(LetBindingsTest, BindingProducerBuiltOnceFansToTwoConsumers) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto deps = makeDeps(store, pool, dispatcher);
  auto terminal = std::make_shared<Terminal>();

  // mid is produced by one Listener; the body references it TWICE as the two
  // inputs of an Aggregate(2). One source event must reach the Aggregate twice.
  rapidjson::Document doc;
  doc.Parse(R"({
    "type":"Let",
    "bindings":{ "mid":{"type":"Listener","streamKey":"SYM","field":"price"} },
    "body":{
      "type":"Aggregate","arity":2,
      "inputs":[ {"type":"Ref","name":"mid"}, {"type":"Ref","name":"mid"} ]
    }
  })");
  ASSERT_FALSE(doc.HasParseError());

  auto root = tree::buildNode(doc, "SYM", deps, terminal);
  ASSERT_TRUE(root);

  dispatcher.notifyListeners("SYM", "price", 5.0);  // a single source event
  pool.shutdown();                                   // drain

  // The lone producer fanned its value to both Aggregate inputs, so the
  // Aggregate(2) fired and emitted both values to the terminal.
  ASSERT_EQ(terminal->size(), 2u);
  EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 5.0);
  EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[1].value), 5.0);

  root->shutdown();
}

TEST(LetBindingsTest, SingleConsumerBindingWorks) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto deps = makeDeps(store, pool, dispatcher);
  auto terminal = std::make_shared<Terminal>();

  rapidjson::Document doc;
  doc.Parse(R"({
    "type":"Let",
    "bindings":{ "p":{"type":"Listener","streamKey":"SYM","field":"price"} },
    "body":{ "type":"Ref","name":"p" }
  })");
  ASSERT_FALSE(doc.HasParseError());

  auto root = tree::buildNode(doc, "SYM", deps, terminal);
  ASSERT_TRUE(root);

  dispatcher.notifyListeners("SYM", "price", 7.0);
  pool.shutdown();

  ASSERT_EQ(terminal->size(), 1u);
  EXPECT_DOUBLE_EQ(std::get<double>(terminal->received[0].value), 7.0);
  root->shutdown();
}

TEST(LetBindingsTest, RefToUnknownBindingThrows) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto deps = makeDeps(store, pool, dispatcher);
  auto terminal = std::make_shared<Terminal>();

  rapidjson::Document doc;
  doc.Parse(R"({
    "type":"Let",
    "bindings":{ "x":{"type":"Listener","streamKey":"SYM","field":"price"} },
    "body":{ "type":"Ref","name":"nope" }
  })");
  EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST(LetBindingsTest, RefOutsideLetThrows) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto deps = makeDeps(store, pool, dispatcher);
  auto terminal = std::make_shared<Terminal>();

  rapidjson::Document doc;
  doc.Parse(R"({"type":"Ref","name":"x"})");
  EXPECT_THROW(tree::buildNode(doc, "SYM", deps, terminal), std::runtime_error);
}

TEST(LetBindingsTest, RejectsMissingBindingsOrBody) {
  rt::ThreadPool pool(1);
  AtomicStore store;
  Dispatcher dispatcher(&pool, &store);
  auto deps = makeDeps(store, pool, dispatcher);
  auto terminal = std::make_shared<Terminal>();

  rapidjson::Document a;
  a.Parse(R"({"type":"Let","body":{"type":"Worker","fn":"last"}})");
  EXPECT_THROW(tree::buildNode(a, "SYM", deps, terminal), std::runtime_error);

  rapidjson::Document b;
  b.Parse(R"({"type":"Let","bindings":{"x":{"type":"Worker","fn":"last"}}})");
  EXPECT_THROW(tree::buildNode(b, "SYM", deps, terminal), std::runtime_error);
}
