// ENC-1335 — exploratory probe (not final).
#include "gma/TreeBuilder.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "../support/CorpusPath.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

using namespace gma;

namespace enc1335_probe {

class Rec final : public gma::INode {
public:
  void onValue(const gma::StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    if (const double* d = std::get_if<double>(&sv.value)) vals_.push_back(*d);
    else ++nonNum_;
  }
  void shutdown() noexcept override {}
  std::vector<double> vals() const { std::lock_guard<std::mutex> lk(mx_); return vals_; }
  std::size_t nonNum() const { std::lock_guard<std::mutex> lk(mx_); return nonNum_; }
private:
  mutable std::mutex mx_;
  std::vector<double> vals_;
  std::size_t nonNum_{0};
};

rapidjson::Document& corpusDoc() {
  static rapidjson::Document doc = [] {
    rapidjson::Document d;
    std::ifstream ifs = gma::testsupport::openCorpusRequests();
    if (!ifs.is_open()) { d.SetNull(); return d; }
    rapidjson::IStreamWrapper isw(ifs);
    d.ParseStream(isw);
    return d;
  }();
  return doc;
}
const rapidjson::Value* corpusRequest(int id, int key) {
  rapidjson::Document& d = corpusDoc();
  if (d.IsNull() || d.HasParseError() || !d.IsArray()) return nullptr;
  for (auto& e : d.GetArray()) {
    if (!e.IsObject() || !e.HasMember("corpus_id")) continue;
    if (e["corpus_id"].GetInt() != id) continue;
    if (!e.HasMember("request")) continue;
    const auto& r = e["request"];
    if (key >= 0 && r.HasMember("key") && r["key"].GetInt() != key) continue;
    return &r;
  }
  return nullptr;
}

void tick(gma::Dispatcher& d, const char* sym, double lastPrice) {
  auto payload = std::make_shared<rapidjson::Document>();
  payload->SetObject();
  auto& al = payload->GetAllocator();
  payload->AddMember("lastPrice", lastPrice, al);
  gma::Event ev;
  ev.symbol = sym;
  ev.payload = payload;
  d.onTick(ev);
}

// drainEvery: drain the pool after every tick (the deterministic control) or
// only once at the end (the live shape).
std::vector<double> drive(const rapidjson::Value& request,
                          const char* symbol,
                          std::size_t ticks,
                          unsigned threads,
                          bool drainEvery) {
  AtomicStore store;
  auto pool = std::make_shared<rt::ThreadPool>(threads);
  auto prev = gThreadPool;
  gThreadPool = pool;

  Dispatcher dispatcher(pool.get(), &store);
  tree::Deps deps;
  deps.store = &store;
  deps.pool = pool.get();
  deps.dispatcher = &dispatcher;

  auto term = std::make_shared<Rec>();
  auto chain = tree::buildForRequest(request, deps, term);

  for (std::size_t n = 0; n < ticks; ++n) {
    tick(dispatcher, symbol, 100.0 + double(n));
    if (drainEvery) pool->drain();
  }
  pool->drain();

  auto out = term->vals();
  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  if (chain.head) chain.head->shutdown();
  chain.keepAlive.clear();
  chain.head.reset();
  pool->shutdown();
  gThreadPool = prev;
  return out;
}

std::string render(const std::vector<double>& v) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(4) << "[" << v.size() << "]";
  for (std::size_t i = 0; i < v.size() && i < 40; ++i) os << " " << v[i];
  return os.str();
}

} // namespace enc1335_probe

TEST(Enc1335Probe, Corpus51Sequence) {
  using namespace enc1335_probe;
  const rapidjson::Value* req = corpusRequest(51, -1);
  ASSERT_NE(req, nullptr);

  constexpr std::size_t kTicks = 12;
  auto control = drive(*req, "AAPL", kTicks, 1, /*drainEvery=*/true);
  std::cout << "CONTROL (drain each tick, threads=1): " << render(control) << "\n";

  for (unsigned th : {1u, 2u, 4u}) {
    int diff = 0;
    for (int rep = 0; rep < 20; ++rep) {
      auto live = drive(*req, "AAPL", kTicks, th, /*drainEvery=*/false);
      if (live != control) {
        if (diff < 3) std::cout << "  threads=" << th << " rep " << rep
                                << " LIVE: " << render(live) << "\n";
        ++diff;
      }
    }
    std::cout << "threads=" << th << " : " << diff << "/20 runs differ from control\n";
  }
}
