// TEMPORARY adversarial probe — ENC-1292 refutation. Delete after use.
#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <cstdio>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
using namespace gma;
namespace {
struct PSink final : public INode {
  mutable std::mutex mx; std::vector<std::pair<std::string,double>> v; std::size_t nonnum{0};
  void onValue(const StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx);
    if (const double* dd = std::get_if<double>(&sv.value)) v.push_back({sv.symbol,*dd});
    else { ++nonnum; v.push_back({sv.symbol+"(REC)",0}); }
  }
  void shutdown() noexcept override {}
};
void probe(const char* label, const char* json,
           const std::vector<std::tuple<std::string,std::string,double>>& ticks,
           const std::vector<std::tuple<std::string,std::string,double>>& presets = {}) {
  rapidjson::Document d; d.Parse(json);
  if (d.HasParseError()) { printf("PROBE [%s] PARSE ERROR\n", label); return; }
  AtomicStore store;
  auto pool = std::make_shared<rt::ThreadPool>(1);
  auto prev = gThreadPool; gThreadPool = pool;
  Dispatcher disp(pool.get(), &store);
  for (auto& [sk,f,val] : presets) store.set(sk,f,val);
  tree::Deps deps; deps.store=&store; deps.pool=pool.get(); deps.dispatcher=&disp;
  auto sink = std::make_shared<PSink>();
  try {
    auto chain = tree::buildForRequest(d, deps, sink);
    for (auto& [sym, fld, val] : ticks) {
      auto payload = std::make_shared<rapidjson::Document>();
      payload->SetObject(); auto& al = payload->GetAllocator();
      payload->AddMember(rapidjson::Value(fld.c_str(), al).Move(),
                         rapidjson::Value(val).Move(), al);
      Event ev; ev.symbol = sym; ev.payload = payload; disp.onTick(ev);
    }
    pool->drain();
    printf("PROBE [%s] ACCEPTED n=%zu:", label, sink->v.size());
    for (auto& [s,x] : sink->v) printf(" %s:%.2f", s.c_str(), x);
    printf("\n");
    for (auto& n : chain.keepAlive) if (n) n->shutdown();
    if (chain.head) chain.head->shutdown();
  } catch (const std::exception& ex) { printf("PROBE [%s] REFUSED: %s\n", label, ex.what()); }
  pool->shutdown(); gThreadPool = prev;
}
std::vector<std::tuple<std::string,std::string,double>> interleaved() {
  std::vector<std::tuple<std::string,std::string,double>> t;
  for (int n=0;n<3;n++){ t.push_back({"AAPL","lastPrice",1000.0+n});
                         t.push_back({"MSFT","lastPrice",5000.0+n}); }
  return t;
}
} // namespace

TEST(ZZTempProbe, AdversarialSweep) {
  auto t = interleaved();
  // control: known-good shape
  probe("ctrl-sk=AAPL-by=none", R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"by":"none","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  // D1: `by` on nodes that are NOT fan-ins
  probe("by-typo-on-Listener", R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "node":{"type":"Listener","streamKey":"AAPL","field":"lastPrice","by":"typo"}})", t);
  probe("by-none-on-Worker-stage", R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "pipeline":[{"type":"Worker","fn":"sma","period":2,"by":"origin"}]})", t);
  // D2: duplicate `by` members
  probe("dup-by none,typo", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"by":"none","by":"typo","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  probe("dup-by typo,none", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"by":"typo","by":"none","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  // D3: by:"none" inside SymbolSplit
  probe("none-inside-SymbolSplit", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"SymbolSplit","child":{"type":"Aggregate","arity":2,"by":"none","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}}})", t);
  probe("default-inside-SymbolSplit", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"SymbolSplit","child":{"type":"Aggregate","arity":2,"inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}}})", t);
  // D4: by inside a Let binding
  probe("none-inside-Let", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Let","bindings":{"x":{"type":"Aggregate","arity":2,"by":"none","inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
          {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}},
        "body":{"type":"Ref","name":"x"}}})", t);
  probe("typo-inside-Let", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Let","bindings":{"x":{"type":"Aggregate","arity":2,"by":"typo","inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
          {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}},
        "body":{"type":"Ref","name":"x"}}})", t);
  // D5: Aggregate as a pipeline stage with by:"none"
  probe("Aggregate-in-pipeline-none", R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "pipeline":[{"type":"Aggregate","arity":2,"by":"none","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}]})", t);
  // D6: nested by:"none" inside by:"none"
  probe("nested-none-in-none", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"by":"none","inputs":[
        {"type":"Aggregate","arity":1,"by":"none","inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"lastPrice"}]},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  // D7: by:"none" on the INNER aggregate only, outer default -> does the outer
  // default now join across what were two streamKeys? (D6 hazard)
  probe("inner-none-outer-default", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"inputs":[
        {"type":"Aggregate","arity":1,"by":"none","inputs":[
          {"type":"Listener","streamKey":"AAPL","field":"lastPrice"}]},
        {"type":"Aggregate","arity":1,"by":"none","inputs":[
          {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}]}})", t);
  // D8: trailing-space / unicode spellings
  probe("by-none-space", R"({"key":1,"streamKey":"AAPL","field":"lastPrice",
      "node":{"type":"Aggregate","arity":1,"by":"none ","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"}]}})", t);
  probe("by-none-unicode", "{\"key\":1,\"streamKey\":\"AAPL\",\"field\":\"lastPrice\","
      "\"node\":{\"type\":\"Aggregate\",\"arity\":1,\"by\":\"n\\u006fne\",\"inputs\":["
      "{\"type\":\"Listener\",\"streamKey\":\"AAPL\",\"field\":\"lastPrice\"}]}}", t);
  // D9: MAX_SYMBOLS-ish: by:"none" arity 1 -> passthrough under request key
  probe("arity1-none-passthrough", R"({"key":1,"streamKey":"ZZZ","field":"lastPrice",
      "node":{"type":"Aggregate","arity":1,"by":"none","inputs":[
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  // D10: Pack by:"none" then Field -> identity
  probe("pack-none-field", R"({"key":1,"streamKey":"PAIR","field":"lastPrice",
      "node":{"type":"Pack","by":"none","fields":{
        "a":{"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        "b":{"type":"Listener","streamKey":"MSFT","field":"lastPrice"}}},
      "pipeline":[{"type":"Field","name":"a"}]})", t);
  // D11: by:"none" where request streamKey is EMPTY-ish whitespace
  probe("sk-whitespace", R"({"key":1,"streamKey":" ","field":"lastPrice",
      "node":{"type":"Aggregate","arity":2,"by":"none","inputs":[
        {"type":"Listener","streamKey":"AAPL","field":"lastPrice"},
        {"type":"Listener","streamKey":"MSFT","field":"lastPrice"}]}})", t);
  SUCCEED();
}
