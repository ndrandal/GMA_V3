// ENC-1335 — A PULL NODE MUST SAMPLE AS OF ITS OWN CLOCK TICK.
// SPEC specs/2026-09-20-gma-join-correctness D3/D4 §1.1; found by ENC-1290's
// refutation pass and reported there as "a race in the `node`-only path that
// no gate covers".
//
// ───────────────────────────────────────────────────────────────────────────
// THE DEFECT
//
// `Dispatcher::onTick` does two things per tick, in this order and both on the
// ingress thread:
//
//     1. write the tick's atomics into the `AtomicStore`
//          (`IEventComputer::compute` -> `MarketTickComputer`, then
//           `Dispatcher::computeAndStoreAtomics` for FunctionMap builtins)
//     2. deliver the clock value to the head `Listener`
//          (INLINE since ENC-1005 — `INode::deliversOnOwnExecutor`)
//
// The Listener hands the clock to the DAG's `rt::Strand`, which runs it on a
// pool worker. The ingress thread does not wait: by the time the worker runs
// tick n's clock, step 1 has already run for ticks n+1, n+2, …
//
// `AtomicAccessor` READS THE STORE and discards the value handed to it. So the
// value it emitted for tick n was the store's content at whatever moment a
// pool worker reached it. The emitted sequence therefore duplicates some
// samples, skips others, and differs between runs of the same binary.
//
// Measured before the fix, corpus 51 ("5-period SMA of AAPL"), 12 ticks,
// 20 repetitions per width, against the per-tick truth read out of the store:
//
//     threads=1  20/20 runs wrong      threads=2  20/20      threads=4  20/20
//
//     truth:  [11]      nan nan nan 102 103 104 105 106 107 108 109
//     run A:  [12]  nan nan nan nan 103 103 105 105 107 107 108 109
//     run B:  [12]      nan nan nan 102 102 105 109 109 109 109 109 109
//
// Note the arrival COUNT moves too (12 where the truth is 11): tick 0 wrote no
// `sma_5` at all, and by the time its clock ran, tick 1 had written one.
//
// ───────────────────────────────────────────────────────────────────────────
// WHY NO EXISTING GATE SEES IT, AND WHY ENC-1005 DID NOT FIX IT
//
// ENC-1289's three gates all drive a FAN-IN request and score TUPLES. This is
// the `node`-only path — 162 of the 272 corpus entries, 81 of which bind an
// `AtomicAccessor` — and its gates are build-only or arrival-count assertions.
//
// ENC-1005's per-request strand (SPEC D3) orders the deliveries of one DAG
// against each other, and it does that correctly here: one arrival per tick, in
// tick order. It cannot move the SAMPLE back to the tick it belongs to, because
// it runs after the ingress thread has gone. **The row above at threads=1 is
// the proof**: a width where ENC-1289's race gate is green by construction,
// and this defect is still 20/20. It is not an ordering defect and an ordering
// fix cannot reach it.
//
// ───────────────────────────────────────────────────────────────────────────
// WHAT THESE TESTS ASSERT, AND IN WHICH ORDER
//
//  1. `PerTickTruthIsReadFromTheStoreAndIsStableAcrossRuns` — the CONTROL.
//     Green before and after the fix. It establishes that there IS a per-tick
//     truth, that it is non-trivial (8 distinct values), and that reading it
//     is deterministic — without which a red test below would prove only that
//     something is wrong, not what.
//
//  2. `EveryClockQueuedBeforeAnyRuns_StillCarriesItsOwnTicksSample` — the
//     DETERMINISTIC gate. No probability, no repetition budget: the pool's one
//     worker is held on a barrier while all 12 ticks are injected, so every
//     store write is finished and every clock is queued before a single
//     delivery runs. Sampling at execution time can then only ever read the
//     LAST tick's value, and the test asserts that the store really did end up
//     there — so the gate cannot pass because the store failed to move.
//
//  3. `LiveIngressSequenceEqualsThePerTickTruth` — the gate in the shape
//     production runs, at three pool widths, with the luck probability stated.
//
//  4. `PullOnlyFanInSamplesBothPortsAsOfOneClockTick` — corpus 111, the
//     `CompositeRoot` half: a join whose two ports are both pull nodes.
//
//  5. `EveryCorpusAtomicAccessorSitsAtAClockHop` — the structural census that
//     says WHY the adjacency rule is sufficient for this corpus (95 of 95), so
//     the green above is not a claim about one entry.
//
//  6. `NoCorpusEntryEmitsADifferentSequenceUnderLiveIngress` — the same
//     comparison driven across all 272 entries.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include "gma/TreeBuilder.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/ThreadPool.hpp"

#include "../support/CorpusPath.hpp"

#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cmath>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace gma;

namespace enc1335 {

// ─── Terminal ──────────────────────────────────────────────────────────────
class Sink final : public gma::INode {
public:
  void onValue(const gma::StreamValue& sv) override {
    std::lock_guard<std::mutex> lk(mx_);
    if (const double* d = std::get_if<double>(&sv.value)) vals_.push_back(*d);
    else                                                  nonNumeric_.push_back(sv.symbol);
  }
  void shutdown() noexcept override {}
  std::vector<double> values() const {
    std::lock_guard<std::mutex> lk(mx_);
    return vals_;
  }
  std::size_t nonNumeric() const {
    std::lock_guard<std::mutex> lk(mx_);
    return nonNumeric_.size();
  }
private:
  mutable std::mutex mx_;
  std::vector<double> vals_;
  std::vector<std::string> nonNumeric_;
};

// ─── Corpus access (same resolver every other corpus-backed suite uses) ────
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

const rapidjson::Value* corpusRequest(int corpusId) {
  rapidjson::Document& d = corpusDoc();
  if (d.IsNull() || d.HasParseError() || !d.IsArray()) return nullptr;
  for (auto& e : d.GetArray()) {
    if (!e.IsObject() || !e.HasMember("corpus_id") || !e["corpus_id"].IsInt()) continue;
    if (e["corpus_id"].GetInt() != corpusId) continue;
    return e.HasMember("request") ? &e["request"] : nullptr;
  }
  return nullptr;
}

// ─── Ticks ─────────────────────────────────────────────────────────────────
// `lastPrice` plus a bid/ask pair around it, and `volume`, so every corpus
// field the sweep may bind is actually carried. `ev.type` is left at its
// default, "tick" — the production event type MarketTickComputer registers for.
void tick(gma::Dispatcher& d, const std::string& symbol, double price) {
  auto payload = std::make_shared<rapidjson::Document>();
  payload->SetObject();
  auto& al = payload->GetAllocator();
  payload->AddMember("lastPrice", price,         al);
  payload->AddMember("bid",       price - 0.01,  al);
  payload->AddMember("ask",       price + 0.01,  al);
  payload->AddMember("volume",    100.0 + price, al);
  gma::Event ev;
  ev.symbol  = symbol;
  ev.payload = payload;
  d.onTick(ev);
}

std::string render(const std::vector<double>& v) {
  std::ostringstream os;
  os << "[" << v.size() << "]" << std::fixed << std::setprecision(4);
  for (std::size_t i = 0; i < v.size() && i < 32; ++i) os << " " << v[i];
  if (v.size() > 32) os << " …";
  return os.str();
}

// NaN-aware. The TA suite writes a NaN `sma_5` for the first few ticks (too
// little history), and that NaN is part of the per-tick truth: a fix that
// smoothed it away would be changing the answer, not fixing the timing.
bool sameValue(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  return a == b;
}
bool sameSequence(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) if (!sameValue(a[i], b[i])) return false;
  return true;
}

// ─── The harness ───────────────────────────────────────────────────────────
//
// One DAG, built by the real `tree::buildForRequest` from the verbatim corpus
// request, driven through the real `Dispatcher::onTick`. Nothing is stubbed,
// subclassed or reimplemented but the terminal, which `buildForRequest` takes
// as a parameter.
enum class Mode {
  DrainEachTick,   // the oracle: the pool is empty before the next tick
  LiveBurst,       // production's shape: ingress does not wait for the DAG
  BlockedPool,     // deterministic: every clock is queued before any runs
};

struct DriveResult {
  std::vector<double>              emitted;
  std::vector<std::optional<double>> storeAfterEachTick;  // the per-tick truth
  std::size_t                      nonNumeric{0};
  bool                             threw{false};
  std::string                      error;
};

// `probe` names an (symbol, field) the caller wants read out of the store on
// the ingress thread immediately after each tick. That read is the independent
// oracle: it is what the store held when the tick's clock was produced, and it
// never passes through the DAG at all.
DriveResult drive(const rapidjson::Value& request,
          const std::vector<std::string>& symbols,
          std::size_t ticks,
          unsigned threads,
          Mode mode,
          std::pair<std::string, std::string> probe = {}) {
  DriveResult out;
  AtomicStore store;
  auto pool = std::make_shared<rt::ThreadPool>(threads);
  auto prevGlobalPool = gThreadPool;
  gThreadPool = pool;

  Dispatcher dispatcher(pool.get(), &store);
  tree::Deps deps;
  deps.store      = &store;
  deps.pool       = pool.get();
  deps.dispatcher = &dispatcher;

  auto sink = std::make_shared<Sink>();
  tree::BuiltChain chain;
  try {
    chain = tree::buildForRequest(request, deps, sink);
  } catch (const std::exception& ex) {
    out.threw = true;
    out.error = ex.what();
    pool->shutdown();
    gThreadPool = prevGlobalPool;
    return out;
  }

  // BlockedPool: occupy every worker until the barrier is released, so no
  // delivery can run while ticks are being injected.
  std::promise<void> release;
  std::shared_future<void> released(release.get_future());
  if (mode == Mode::BlockedPool) {
    for (unsigned i = 0; i < threads; ++i)
      pool->post([released]() { released.wait(); });
  }

  for (std::size_t n = 0; n < ticks; ++n) {
    for (const auto& s : symbols) tick(dispatcher, s, 100.0 + double(n));
    if (!probe.first.empty()) {
      auto v = store.get(probe.first, probe.second);
      if (v.has_value()) {
        if (const double* d = std::get_if<double>(&v.value())) out.storeAfterEachTick.emplace_back(*d);
        else                                                   out.storeAfterEachTick.emplace_back(std::nullopt);
      } else {
        out.storeAfterEachTick.emplace_back(std::nullopt);
      }
    }
    if (mode == Mode::DrainEachTick) pool->drain();
  }

  if (mode == Mode::BlockedPool) release.set_value();
  pool->drain();

  out.emitted    = sink->values();
  out.nonNumeric = sink->nonNumeric();

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  if (chain.head) chain.head->shutdown();
  chain.keepAlive.clear();
  chain.head.reset();
  pool->shutdown();
  gThreadPool = prevGlobalPool;
  return out;
}

// The truth as a plain sequence: the store's value at each tick, with the ticks
// that had no value at all omitted — those must produce no arrival.
std::vector<double> truthSequence(const DriveResult& r) {
  std::vector<double> out;
  for (const auto& v : r.storeAfterEachTick) if (v.has_value()) out.push_back(*v);
  return out;
}

// corpus 51: {streamKey AAPL, field lastPrice, node AtomicAccessor(AAPL,sma_5)}
constexpr int         kCorpus51  = 51;
constexpr std::size_t kTicks     = 12;
const std::vector<std::string> kAAPL{"AAPL"};
const std::pair<std::string, std::string> kProbe51{"AAPL", "sma_5"};

// ─── Structural census (test 5) ────────────────────────────────────────────
// Where does an `AtomicAccessor` sit in a request? The two CLOCK HOPS — the
// only places a bind can be taken — are
//   * the head `Listener`'s immediate downstream, i.e. the head of `node`, and
//   * a declared input of a fan-in `node`, which `CompositeRoot` clocks.
enum class Placement { NodeHead, FanInInput, Deeper };

void censusWalk(const rapidjson::Value& n,
                int depth,
                bool underDeclaredInputs,
                std::map<Placement, int>& out) {
  if (!n.IsObject()) return;
  if (n.HasMember("type") && n["type"].IsString() &&
      std::string(n["type"].GetString()) == "AtomicAccessor") {
    if (depth == 0)                                  ++out[Placement::NodeHead];
    else if (depth == 1 && underDeclaredInputs)      ++out[Placement::FanInInput];
    else                                             ++out[Placement::Deeper];
  }
  for (const char* k : {"inputs", "stages", "cases", "branches"}) {
    if (n.HasMember(k) && n[k].IsArray())
      for (auto& c : n[k].GetArray())
        censusWalk(c, depth + 1, std::string(k) == "inputs", out);
  }
  for (const char* k : {"body", "input", "node"}) {
    if (n.HasMember(k) && n[k].IsObject()) censusWalk(n[k], depth + 1, false, out);
  }
  if (n.HasMember("fields") && n["fields"].IsObject())
    for (auto it = n["fields"].MemberBegin(); it != n["fields"].MemberEnd(); ++it)
      censusWalk(it->value, depth + 1, true, out);
  if (n.HasMember("bindings") && n["bindings"].IsObject())
    for (auto it = n["bindings"].MemberBegin(); it != n["bindings"].MemberEnd(); ++it)
      censusWalk(it->value, depth + 1, false, out);
}

std::vector<std::string> requestStreamKeys(const rapidjson::Value& v,
                                           int depth = 0) {
  std::vector<std::string> out;
  if (depth > 24) return out;
  if (v.IsObject()) {
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
      if (std::string(it->name.GetString()) == "streamKey" && it->value.IsString())
        out.emplace_back(it->value.GetString());
      auto sub = requestStreamKeys(it->value, depth + 1);
      out.insert(out.end(), sub.begin(), sub.end());
    }
  } else if (v.IsArray()) {
    for (auto& c : v.GetArray()) {
      auto sub = requestStreamKeys(c, depth + 1);
      out.insert(out.end(), sub.begin(), sub.end());
    }
  }
  return out;
}

} // namespace enc1335

// ═══ 1. THE CONTROL ════════════════════════════════════════════════════════
//
// GREEN BEFORE AND AFTER THE FIX, and it has to be: it is the only thing that
// makes a red elsewhere mean "the sample was taken at the wrong time" rather
// than "the oracle is broken".
//
// Two claims. (a) There is a per-tick truth and reading it is deterministic:
// the store's `sma_5` after each of 12 ticks is the same sequence on every
// run. (b) That truth is NOT TRIVIAL — 8 distinct finite values and a 3-tick
// NaN prefix, with tick 0 holding no `sma_5` at all. A constant truth would
// make every test below pass under any sampling rule whatsoever.
TEST(ClockCoherentPull, PerTickTruthIsReadFromTheStoreAndIsStableAcrossRuns) {
  using namespace enc1335;
  const rapidjson::Value* req = corpusRequest(kCorpus51);
  ASSERT_NE(req, nullptr) << "corpus_id 51 not found: "
                          << gma::testsupport::corpusNotFoundDiagnostic();

  std::vector<double> first;
  for (int rep = 0; rep < 5; ++rep) {
    DriveResult r = drive(*req, kAAPL, kTicks, /*threads=*/1, Mode::DrainEachTick, kProbe51);
    ASSERT_FALSE(r.threw) << r.error;
    ASSERT_EQ(r.storeAfterEachTick.size(), kTicks);
    const auto truth = truthSequence(r);
    if (rep == 0) first = truth;
    EXPECT_TRUE(sameSequence(truth, first))
        << "the ORACLE is not deterministic, so nothing below can be trusted.\n"
           "    rep 0: " << render(first) << "\n    rep " << rep << ": "
        << render(truth);

    // The DAG, drained after every tick, must reproduce the oracle exactly.
    // This is the sampling rule stated positively: one arrival per tick that
    // had a value, carrying that value.
    EXPECT_TRUE(sameSequence(r.emitted, truth))
        << "with the pool drained after every tick there is no window to race "
           "in, so the terminal must see exactly the store's per-tick values.\n"
           "    truth:   " << render(truth) << "\n    emitted: "
        << render(r.emitted);
  }

  // (b) anti-vacuity: the truth must be able to tell two sampling rules apart.
  std::size_t nans = 0, absent = 0;
  std::set<long long> distinctFinite;
  for (const auto& v : {0}) { (void)v; }
  {
    DriveResult r = drive(*corpusRequest(kCorpus51), kAAPL, kTicks, 1,
                  Mode::DrainEachTick, kProbe51);
    for (const auto& v : r.storeAfterEachTick) {
      if (!v.has_value())          ++absent;
      else if (std::isnan(*v))     ++nans;
      else distinctFinite.insert(std::llround(*v * 10000.0));
    }
  }
  EXPECT_EQ(absent, 1u) << "tick 0 writes no sma_5 (one price, period 5), so "
                           "exactly one tick must have no value to sample";
  EXPECT_EQ(nans, 3u)   << "ticks 1-3 write a NaN sma_5; that NaN is part of "
                           "the truth and must not be smoothed away";
  EXPECT_GE(distinctFinite.size(), 8u)
      << "the per-tick truth must take at least 8 DISTINCT values over "
      << kTicks << " ticks, or a wrong sampling rule could reproduce it by "
         "accident. Distinct values seen: " << distinctFinite.size();
}

// ═══ 2. THE DETERMINISTIC GATE ═════════════════════════════════════════════
//
// No probability and no repetition budget — the interleaving is FORCED.
//
// The pool's single worker is parked on a barrier before the first tick, so
// all 12 ticks complete their store writes and queue all 12 clock deliveries
// before one of them runs. Then the barrier is released.
//
// Under execution-time sampling every delivery reads the store after the last
// write, so every arrival is the LAST tick's `sma_5` — 11 copies of 109.0.
// Under clock-time sampling each delivery carries the value its own tick
// wrote.
//
// IT ASSERTS THE STORE ACTUALLY MOVED. `EXPECT_EQ(storeAtEnd, last tick's
// value)` and `EXPECT_GE(distinct, 8)` are what stop this passing because
// nothing was ever overwritten — which is the way a gate like this fails open.
TEST(ClockCoherentPull, EveryClockQueuedBeforeAnyRuns_StillCarriesItsOwnTicksSample) {
  using namespace enc1335;
  const rapidjson::Value* req = corpusRequest(kCorpus51);
  ASSERT_NE(req, nullptr) << gma::testsupport::corpusNotFoundDiagnostic();

  DriveResult oracle = drive(*req, kAAPL, kTicks, 1, Mode::DrainEachTick, kProbe51);
  ASSERT_FALSE(oracle.threw) << oracle.error;
  const auto truth = truthSequence(oracle);

  DriveResult blocked = drive(*req, kAAPL, kTicks, 1, Mode::BlockedPool, kProbe51);
  ASSERT_FALSE(blocked.threw) << blocked.error;

  // The store genuinely advanced while the deliveries sat in the queue.
  ASSERT_EQ(blocked.storeAfterEachTick.size(), kTicks);
  ASSERT_TRUE(blocked.storeAfterEachTick.back().has_value());
  ASSERT_TRUE(truth.size() >= 2);
  EXPECT_TRUE(sameValue(*blocked.storeAfterEachTick.back(), truth.back()))
      << "the store's final sma_5 must be the last tick's, or the premise of "
         "this gate (that later writes happened before any delivery ran) is "
         "false";

  std::set<long long> distinct;
  for (double v : blocked.emitted) if (!std::isnan(v)) distinct.insert(std::llround(v * 10000.0));

  EXPECT_TRUE(sameSequence(blocked.emitted, truth))
      << "every clock for ticks 0.." << (kTicks - 1) << " was queued behind a "
         "parked pool worker before any of them ran, so the store had already "
         "advanced to the LAST tick when the first delivery executed.\n"
         "    Each delivery must still carry the value ITS OWN tick wrote.\n"
         "    truth:   " << render(truth) << "\n    emitted: "
      << render(blocked.emitted)
      << "\n\n    A sequence of " << blocked.emitted.size()
      << " copies of the final value is this defect exactly: the sample was "
         "taken when a pool worker reached the node, not when the tick that "
         "clocked it was produced. See INode::samplesAtClock.";

  EXPECT_GE(distinct.size(), 8u)
      << "emitted " << distinct.size() << " distinct finite values; the truth "
         "has at least 8. One distinct value means every sample was read after "
         "the last store write: " << render(blocked.emitted);
}

// ═══ 3. THE GATE IN PRODUCTION'S SHAPE ═════════════════════════════════════
//
// Ingress does not wait for the DAG, which is how the server runs.
//
// THE LUCK BUDGET, STATED. Pre-fix this test was red on 20 of 20 runs at each
// of threads=1, 2 and 4 — 60 of 60, no counterexample — so the per-run failure
// probability p is not distinguishable from 1 at this budget, and the chance of
// a clean pass by luck across the three widths is below (1-0.95)^3 ~ 1e-4 on
// the most pessimistic reading of 20/20 (a 95% lower confidence bound on p).
// With 4 reps per width it is (1-0.95)^12 ~ 2e-16. That is the neighbourhood
// ENC-1289 set as the bar (1e-64 for its race gate; "anything near 1 in 50 is
// not a gate").
//
// It is NOT a tail-probability gate the way ENC-1289's was, and that is an
// improvement rather than a weakness: there is no narrow window to hit. The
// ingress thread outruns the pool on essentially every tick, so a wrong
// sampling rule is wrong almost everywhere. Test 2 removes the "essentially".
TEST(ClockCoherentPull, LiveIngressSequenceEqualsThePerTickTruth) {
  using namespace enc1335;
  const rapidjson::Value* req = corpusRequest(kCorpus51);
  ASSERT_NE(req, nullptr) << gma::testsupport::corpusNotFoundDiagnostic();

  DriveResult oracle = drive(*req, kAAPL, kTicks, 1, Mode::DrainEachTick, kProbe51);
  ASSERT_FALSE(oracle.threw) << oracle.error;
  const auto truth = truthSequence(oracle);
  ASSERT_GE(truth.size(), 2u);

  constexpr int kReps = 4;
  for (unsigned threads : {1u, 2u, 4u}) {
    for (int rep = 0; rep < kReps; ++rep) {
      DriveResult live = drive(*req, kAAPL, kTicks, threads, Mode::LiveBurst, kProbe51);
      ASSERT_FALSE(live.threw) << live.error;
      EXPECT_TRUE(sameSequence(live.emitted, truth))
          << "threads=" << threads << " rep=" << rep
          << ": the ingress thread injected " << kTicks << " ticks without "
             "waiting for the DAG, which is how the server runs.\n"
             "    truth:   " << render(truth) << "\n    emitted: "
          << render(live.emitted)
          << "\n\n    Duplicated or skipped samples here are ENC-1335: the "
             "pull was read at the pool's convenience rather than at its own "
             "clock tick. ENC-1005's strand does not reach this — note that "
             "threads=1 is affected identically.";
    }
  }
}

// ═══ 4. THE FAN-IN HALF — `CompositeRoot` FORWARDS THE BIND ════════════════
//
// corpus 111 declares an `Aggregate` whose two inputs are both
// `AtomicAccessor`s. They have no Dispatcher subscription of their own, so
// ENC-1290's clock rule is what makes them fire at all — through
// `CompositeRoot::onValue`, one hop past the head `Listener`.
//
// That makes `CompositeRoot` a clock hop, and it must forward the bind: it
// computes nothing, buffers nothing and drops nothing, so "as of this tick"
// means the same on either side of it. Without the forwarding the two ports
// are sampled at two unrelated moments and then JOINED — the same race, in the
// one shape where it also corrupts the pairing.
TEST(ClockCoherentPull, PullOnlyFanInSamplesBothPortsAsOfOneClockTick) {
  using namespace enc1335;
  const rapidjson::Value* req = corpusRequest(111);
  ASSERT_NE(req, nullptr) << gma::testsupport::corpusNotFoundDiagnostic();

  DriveResult oracle = drive(*req, requestStreamKeys(*req), kTicks, 1, Mode::DrainEachTick);
  ASSERT_FALSE(oracle.threw) << oracle.error;
  ASSERT_FALSE(oracle.emitted.empty())
      << "corpus 111's pull-only join must EMIT — ENC-1290 clocked it into "
         "life, and if it is silent this test is measuring nothing";

  DriveResult blocked = drive(*req, requestStreamKeys(*req), kTicks, 1, Mode::BlockedPool);
  ASSERT_FALSE(blocked.threw) << blocked.error;
  EXPECT_TRUE(sameSequence(blocked.emitted, oracle.emitted))
      << "every clock was queued before any delivery ran, so both ports must "
         "still carry the values their own tick wrote.\n"
         "    drained-per-tick: " << render(oracle.emitted)
      << "\n    all-clocks-queued: " << render(blocked.emitted)
      << "\n\n    A difference here means CompositeRoot delivered the clock "
         "and let each AtomicAccessor read the store later — see the "
         "bindAtClock override on CompositeRoot in src/core/TreeBuilder.cpp.";
}

// ═══ 5. WHY ADJACENCY IS ENOUGH — THE CENSUS ═══════════════════════════════
//
// The fix binds at two hops and nowhere else, deliberately: binding a pull
// node further down a chain would mean evaluating a `Filter`'s predicate or a
// `Worker`'s accumulator on the ingress thread, which is the inline-the-DAG
// design SPEC D3's cheap path was struck for.
//
// So the green above is a claim about this corpus, and this test is the claim.
// If a future corpus edit authors a deeper placement, this NAMES it instead of
// leaving the gap to be found a month later.
TEST(ClockCoherentPull, EveryCorpusAtomicAccessorSitsAtAClockHop) {
  using namespace enc1335;
  rapidjson::Document& doc = corpusDoc();
  ASSERT_FALSE(doc.IsNull()) << gma::testsupport::corpusNotFoundDiagnostic();
  ASSERT_TRUE(doc.IsArray());
  EXPECT_EQ(doc.Size(), 272u)
      << "the corpus size moved; re-measure the 95-of-95 census quoted in "
         "include/gma/nodes/INode.hpp rather than editing this number";

  std::map<Placement, int> census;
  std::vector<std::string> deeper;
  for (auto& e : doc.GetArray()) {
    if (!e.IsObject() || !e.HasMember("request")) continue;
    const int id = (e.HasMember("corpus_id") && e["corpus_id"].IsInt())
                     ? e["corpus_id"].GetInt() : -1;
    const auto& r = e["request"];
    std::map<Placement, int> one;
    if (r.HasMember("node") && r["node"].IsObject()) censusWalk(r["node"], 0, false, one);
    for (const char* k : {"pipeline", "stages"}) {
      if (r.HasMember(k) && r[k].IsArray())
        for (auto& st : r[k].GetArray()) censusWalk(st, 1, false, one);
    }
    for (auto& [p, n] : one) census[p] += n;
    if (one[Placement::Deeper] > 0)
      deeper.push_back("corpus_id " + std::to_string(id) + ": " +
                       std::to_string(one[Placement::Deeper]) + " deeper");
  }

  EXPECT_EQ(census[Placement::NodeHead], 81)
      << "81 AtomicAccessors sit at the head of `node`, directly downstream of "
         "the head Listener";
  EXPECT_EQ(census[Placement::FanInInput], 14)
      << "14 sit as declared inputs of the 7 pull-only fan-ins (ids 111-115, "
         "192, 200), which CompositeRoot clocks";

  std::string detail;
  for (const auto& d : deeper) detail += "\n  " + d;
  EXPECT_TRUE(deeper.empty())
      << deeper.size() << " corpus entr(ies) place an AtomicAccessor deeper "
         "than a clock hop. Those keep execution-time sampling — ENC-1335 does "
         "NOT fix them, and the 95-of-95 reach claim no longer holds:" << detail;

  EXPECT_EQ(census[Placement::NodeHead] + census[Placement::FanInInput], 95)
      << "the census must account for all 95 occurrences";
}

// ═══ 6. THE SAME COMPARISON, ACROSS ALL 272 ════════════════════════════════
//
// The per-entry claim driven over the whole corpus: for every entry that emits
// anything, the live-ingress sequence must equal the drained-per-tick one.
//
// Entries whose DAG is timer-driven (`Interval`, `BucketTime`, 24 of them) are
// EXCLUDED and counted, not silently passed: their clock is a wall-clock timer,
// so two runs legitimately differ in how many pulses land inside the test and
// there is no tick for the sample to be coherent WITH. That is the residual
// this ticket does not fix, named with a number rather than left implicit.
TEST(ClockCoherentPull, NoCorpusEntryEmitsADifferentSequenceUnderLiveIngress) {
  using namespace enc1335;
  rapidjson::Document& doc = corpusDoc();
  ASSERT_FALSE(doc.IsNull()) << gma::testsupport::corpusNotFoundDiagnostic();
  ASSERT_TRUE(doc.IsArray());

  constexpr std::size_t kSweepTicks = 8;
  std::size_t checked = 0, emitted = 0, timerDriven = 0, built = 0, refused = 0;
  // ANTI-VACUITY. An entry whose drained sequence is CONSTANT cannot tell two
  // sampling rules apart: a mistimed sample of an unchanging value is the same
  // value. `differing == 0` is therefore only meaningful alongside a floor on
  // how many entries could have differed at all. Over an 8-tick window most of
  // the long-period TA fields (sma_20, sma_50, ema_50, atr_14, …) are absent or
  // NaN throughout, which is exactly why the measured verdict change is ~35 of
  // the 88 entries that structurally bind an AtomicAccessor and not all 88.
  std::size_t sensitive = 0;
  std::vector<std::string> differing;

  for (auto& e : doc.GetArray()) {
    if (!e.IsObject() || !e.HasMember("request")) continue;
    const int id = (e.HasMember("corpus_id") && e["corpus_id"].IsInt())
                     ? e["corpus_id"].GetInt() : -1;
    const auto& r = e["request"];

    // Timer-driven: excluded and counted.
    {
      rapidjson::StringBuffer sb;
      rapidjson::Writer<rapidjson::StringBuffer> w(sb);
      r.Accept(w);
      const std::string json(sb.GetString(), sb.GetSize());
      if (json.find("\"Interval\"") != std::string::npos ||
          json.find("\"BucketTime\"") != std::string::npos ||
          json.find("\"TumblingWindow\"") != std::string::npos) {
        ++timerDriven;
        continue;
      }
    }

    const auto syms = requestStreamKeys(r);
    if (syms.empty()) continue;

    DriveResult a = drive(r, syms, kSweepTicks, 1, Mode::DrainEachTick);
    if (a.threw) { ++refused; continue; }
    ++built;
    DriveResult b = drive(r, syms, kSweepTicks, 1, Mode::LiveBurst);
    ASSERT_FALSE(b.threw) << "corpus " << id << " built once and threw once: "
                          << b.error;
    ++checked;
    if (!a.emitted.empty()) ++emitted;
    {
      bool varies = false;
      for (std::size_t i = 1; i < a.emitted.size(); ++i)
        if (!sameValue(a.emitted[i], a.emitted[0])) { varies = true; break; }
      if (varies) ++sensitive;
    }
    if (!sameSequence(a.emitted, b.emitted) && differing.size() < 12) {
      std::ostringstream os;
      os << "\n  corpus_id " << id
         << "\n     drained: " << render(a.emitted)
         << "\n     live:    " << render(b.emitted);
      differing.push_back(os.str());
    } else if (!sameSequence(a.emitted, b.emitted)) {
      differing.push_back("\n  corpus_id " + std::to_string(id));
    }
  }

  std::cout << "[ENC-1335 sweep] checked=" << checked << " emitting=" << emitted
            << " timer-driven(excluded)=" << timerDriven
            << " build-refused=" << refused
            << " sensitive(non-constant drained sequence)=" << sensitive
            << " differing=" << differing.size() << "\n";

  EXPECT_GE(emitted, 150u)
      << "only " << emitted << " of " << checked << " entries emitted "
         "anything; a sweep over silent DAGs proves nothing";
  EXPECT_GE(sensitive, 30u)
      << "only " << sensitive << " of " << checked << " entries emit a "
         "NON-CONSTANT sequence over " << kSweepTicks
         << " ticks. A mistimed sample of a constant value is the same value, "
            "so `differing == 0` below would be vacuous. Measured at 36 when "
            "this test was written.";

  std::string detail;
  for (const auto& d : differing) detail += d;
  EXPECT_TRUE(differing.empty())
      << differing.size() << " of " << checked
      << " corpus entries emit a different sequence when ingress does not wait "
         "for the DAG than when it does. Each one is a value whose sample was "
         "taken at the pool's convenience:" << detail;
}
