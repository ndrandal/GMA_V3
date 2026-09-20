#include "gma/TreeBuilder.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/AtomicStore.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/Listener.hpp"
#include "gma/Event.hpp"
#include "gma/StreamValue.hpp"
#include <gtest/gtest.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace gma;

// Stub terminal node
class CorpusTerminal : public INode {
public:
    void onValue(const StreamValue&) override {}
    void shutdown() noexcept override {}
};

class CorpusTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        gThreadPool = std::make_shared<rt::ThreadPool>(2);
        dispatcher = std::make_unique<Dispatcher>(gThreadPool.get(), &store);
        deps.store = &store;
        deps.pool = gThreadPool.get();
        deps.dispatcher = dispatcher.get();
    }

    void TearDown() override {
        if (dispatcher) dispatcher.reset();
        if (gThreadPool) {
            gThreadPool->shutdown();
            gThreadPool.reset();
        }
    }

    AtomicStore store;
    std::unique_ptr<Dispatcher> dispatcher;
    tree::Deps deps;
};

TEST_F(CorpusTestFixture, AllCorpusRequestsBuild) {
    // Try multiple search paths for the corpus file
    std::string paths[] = {
        "corpus_requests.json",
        "../tests/treebuilder/corpus_requests.json",
        "tests/treebuilder/corpus_requests.json",
    };

    std::ifstream ifs;
    std::string usedPath;
    for (const auto& p : paths) {
        ifs.open(p);
        if (ifs.is_open()) {
            usedPath = p;
            break;
        }
    }

    if (!ifs.is_open()) {
        // ENC-807 (L17): a missing corpus must NOT silently green the suite.
        // CMake copies corpus_requests.json next to the test binary (cwd), so a
        // miss here is a real build/packaging failure, not a skip.
        FAIL() << "corpus_requests.json not found next to the test binary "
                  "(cwd) — the corpus must be present for this test to run. "
                  "Searched: corpus_requests.json, "
                  "../tests/treebuilder/corpus_requests.json, "
                  "tests/treebuilder/corpus_requests.json";
        return;
    }

    rapidjson::IStreamWrapper isw(ifs);
    rapidjson::Document doc;
    doc.ParseStream(isw);
    ifs.close();

    ASSERT_FALSE(doc.HasParseError()) << "Failed to parse corpus JSON";
    ASSERT_TRUE(doc.IsArray()) << "Corpus must be a JSON array";

    int passed = 0;
    int failed = 0;
    int total = doc.Size();

    for (rapidjson::SizeType i = 0; i < doc.Size(); ++i) {
        const auto& entry = doc[i];
        int corpusId = entry.HasMember("corpus_id") ? entry["corpus_id"].GetInt() : -1;
        std::string nl = entry.HasMember("nl") ? entry["nl"].GetString() : "";

        ASSERT_TRUE(entry.HasMember("request")) << "Entry " << i << " missing 'request'";
        const auto& req = entry["request"];

        auto terminal = std::make_shared<CorpusTerminal>();

        try {
            auto chain = tree::buildForRequest(req, deps, terminal);
            EXPECT_NE(chain.head, nullptr)
                << "Corpus #" << corpusId << " built null head: " << nl;

            // Shut down to clean up Listener/Interval threads
            if (chain.head) chain.head->shutdown();
            for (auto& node : chain.keepAlive) {
                if (node) node->shutdown();
            }

            ++passed;
        } catch (const std::exception& e) {
            ADD_FAILURE() << "Corpus #" << corpusId << " FAILED: " << e.what()
                          << "\n  NL: " << nl;
            ++failed;
        }
    }

    std::cout << "\n[CorpusTest] " << passed << "/" << total << " requests built successfully\n";
    if (failed > 0) {
        std::cout << "[CorpusTest] " << failed << " FAILURES\n";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// ENC-1289 — real value assertions (SPEC specs/2026-09-20-gma-join-correctness
// D8, §1.2).
//
// Everything above this line asserts `chain.head != nullptr`. That is a build
// smoke test: it cannot distinguish a join that computes the right answer from
// one that computes `+1.00` where the truth is `0.02`. What follows drives a
// deterministic tick sequence into the *real* DAG of a named corpus entry and
// asserts what comes out.
//
// WHAT IS REAL HERE. The DAG is built by `tree::buildForRequest` from the
// verbatim `request` object of the corpus file — the same entry, read from the
// same file, that `AllCorpusRequestsBuild` iterates. Values enter through
// `Dispatcher::onTick`. No node is stubbed, subclassed or reimplemented; the
// single substitution is the terminal, which `buildForRequest` takes as a
// parameter and which `AllCorpusRequestsBuild` already substitutes.
//
// ───────────────────────────────────────────────────────────────────────────
// THE CONCURRENCY PROBLEM, AND HOW THESE TESTS HANDLE IT
//
// SPEC §1.1 names four defects. Two of them are *state-machine* defects and
// two are *ordering* defects, and they need opposite test strategies:
//
//   defect 2 — `Aggregate` counts VALUES, not distinct inputs
//              (src/nodes/Aggregate.cpp:30-32: `buf_[sv.symbol].vals` grows and
//              fires at `>= arity_`), so a two-input node completes a "tuple"
//              from two values of ONE input.
//   defect 3 — the correlation key is `sv.symbol`, so no cross-streamKey join
//              exists at all; two symbols occupy two independent buffers.
//   defect 4 — `Listener::onValue` posts every value to a shared `ThreadPool`
//              queue, so two ticks from one Listener execute concurrently and
//              the join pairs across ticks.
//
// Defects 2 and 3 are properties of `Aggregate`'s buffer, held under its own
// mutex. They do NOT depend on scheduling: the counting is the same at one
// thread as at sixteen. So the tests for them run at **threads=1** and are
// exactly reproducible — no seed, no repetition budget, no flake.
//
// Defect 4 is a race, and it is the trap in this ticket. At **threads=1** the
// join is correct BY CONSTRUCTION: `ThreadPool`'s queue is FIFO, one worker
// drains it in order, and `Dispatcher::onTick`'s inner listener container is a
// `std::map`, so `ask` is notified before `bid` deterministically (`"ask" <
// "bid"`). The committed probe at
// specs/2026-09-20-gma-join-correctness/probe/ measured 0 wrong tuples in
// 1,200,000 consecutive single-threaded tuples. **A value assertion written at
// one thread is therefore green by construction against the very defect it
// names.**
//
// So the value assertion is written twice, deliberately:
//
//   `Corpus86_SpreadIsExactlyTwoCents_SingleThreadControl` runs the identical
//   driver and the identical checker at threads=1. It PASSES today. Its job is
//   to prove the checker is capable of returning "clean" — a checker that
//   always reports corruption gates nothing — and to pin, in the suite itself,
//   the fact that one thread hides this defect completely.
//
//   `Corpus86_SpreadIsExactlyTwoCents` runs the same checker at threads=4 —
//   the worst width the probe measured (pooled 6.62-7.27% wrong tuples over
//   1.2M tuples; per-1000-tick-run 1.70%-13.80%, minimum over 1,200 runs
//   1.70%, never zero). It FAILS today.
//
// WHAT THE THREAD-COUNT CHOICE COVERS, AND WHAT IT DOES NOT. Four threads is
// one point on a curve that is NOT monotonic (probe: 2 threads 2.05%, 4
// threads 6.66%, 8 threads 3.06%, 16 threads 2.62%). These tests do not sweep
// widths — that is the probe's job, and the probe is committed for exactly
// that reason. They pick the width with the largest measured effect so the
// gate's detection probability is highest, and they say so here rather than
// implying the choice was principled beyond that.
//
// THE REPETITION BUDGET. `kRaceReps` x `kRaceTicks` ticks yields
// `kRaceReps * kRaceTicks` tuples. Taking the probe's *minimum* observed
// per-run rate (1.70%) as a pessimistic p, the probability this gate sees zero
// wrong tuples and passes by luck is (1 - 0.017)^(reps*ticks): at 4 x 1000
// that is e^-68, i.e. ~2e-30. Even at a p of 0.001 — sixteen times below
// anything measured — it is e^-4 ~ 1.8%. The budget is stated, not assumed.
//
// WHAT IS NOT COVERED HERE. SPEC §1.1 defect 1 (a request carrying BOTH `node`
// and `pipeline` builds two live chains into one terminal) has no assertion
// below. Its correct shape is `Q1`, still open in the SPEC, and it blocks
// ENC-1290; pinning an expectation before Q1 is answered would pin a guess.
// The tests below sidestep it instead: corpus 86 is driven with `bid`/`ask`
// only (never `lastPrice`), which leaves the outer `Listener(AAPL,lastPrice)`
// silent and makes the terminal stream unambiguously the join's output.
//
// ───────────────────────────────────────────────────────────────────────────
// THIS FILE IS RED ON master, AND THAT IS THE POINT
//
// SPEC D8: "an instrument that cannot fail is not a gate. This is sequenced
// first deliberately." Each assertion below names the ticket that turns it
// green:
//
//   Corpus86_JoinMustNotCompleteFromOneInputAlone  -> ENC-1291 (port-indexed
//                                                     fan-in + arity, D2)
//   Corpus86_SpreadIsExactlyTwoCents               -> ENC-1005 (per-request
//                                                     strand, D3/D4)
//   Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself
//                                                  -> ENC-1291, and ENC-1292
//                                                     (declared `by`, D1)
// ═══════════════════════════════════════════════════════════════════════════

namespace corpus_values {

// ─── Recording terminal ────────────────────────────────────────────────────
// `Aggregate::onValue` takes the completed batch under its mutex and then
// forwards its members one at a time, OUTSIDE the lock, from a single thread
// in a tight loop (src/nodes/Aggregate.cpp:39-46). So the terminal's
// per-thread arrival sequence is exactly a concatenation of arity-sized
// batches: two batches forwarded concurrently are on two threads and cannot
// interleave. Stamping each arrival with the forwarding thread is what makes
// tuples recoverable at all. Every recovery below asserts each per-thread run
// has even length — if that ever fails the pairing is unsound and the verdict
// must be discarded, not believed.
class RecordingTerminal final : public gma::INode {
public:
  void onValue(const gma::StreamValue& sv) override {
    const double* d = std::get_if<double>(&sv.value);
    std::lock_guard<std::mutex> lk(mx_);
    if (d) byThread_[std::this_thread::get_id()].push_back(*d);
    else   ++nonNumeric_;
  }
  void shutdown() noexcept override {}

  std::map<std::thread::id, std::vector<double>> take() {
    std::lock_guard<std::mutex> lk(mx_);
    auto out = std::move(byThread_);
    byThread_.clear();
    return out;
  }
  std::size_t nonNumeric() const {
    std::lock_guard<std::mutex> lk(mx_);
    return nonNumeric_;
  }

private:
  mutable std::mutex mx_;
  std::map<std::thread::id, std::vector<double>> byThread_;
  std::size_t nonNumeric_{0};
};

// ─── Corpus access ─────────────────────────────────────────────────────────
// Same search paths as AllCorpusRequestsBuild above; CMake copies the corpus
// next to the test binary and runs the suite from that directory.
rapidjson::Document& corpusDoc() {
  static rapidjson::Document doc = [] {
    const char* paths[] = {
      "corpus_requests.json",
      "../tests/treebuilder/corpus_requests.json",
      "tests/treebuilder/corpus_requests.json",
    };
    rapidjson::Document d;
    for (const char* p : paths) {
      std::ifstream ifs(p);
      if (!ifs.is_open()) continue;
      rapidjson::IStreamWrapper isw(ifs);
      d.ParseStream(isw);
      return d;
    }
    d.SetNull();
    return d;
  }();
  return doc;
}

// Returns nullptr when the corpus is missing/unparseable or the id is absent —
// callers ASSERT on it, so a packaging failure is loud rather than a skip.
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

// ─── Driver ────────────────────────────────────────────────────────────────
struct Drive {
  std::map<std::thread::id, std::vector<double>> byThread;
  std::size_t arrivals{0};
  std::size_t nonNumeric{0};
};

Drive driveCorpus(const rapidjson::Value& request,
                  unsigned threads,
                  const std::function<void(gma::Dispatcher&)>& inject) {
  using namespace gma;

  AtomicStore store;
  auto pool = std::make_shared<rt::ThreadPool>(threads);
  auto prevPool = gThreadPool;
  gThreadPool = pool;

  Dispatcher dispatcher(pool.get(), &store);
  tree::Deps deps;
  deps.store      = &store;
  deps.pool       = pool.get();
  deps.dispatcher = &dispatcher;

  auto terminal = std::make_shared<RecordingTerminal>();
  auto chain    = tree::buildForRequest(request, deps, terminal);

  inject(dispatcher);
  pool->drain();

  Drive out;
  out.byThread   = terminal->take();
  out.nonNumeric = terminal->nonNumeric();
  for (auto& [tid, vals] : out.byThread) { (void)tid; out.arrivals += vals.size(); }

  for (auto& n : chain.keepAlive) if (n) n->shutdown();
  if (chain.head) chain.head->shutdown();
  chain.keepAlive.clear();
  chain.head.reset();
  pool->shutdown();
  gThreadPool = prevPool;
  return out;
}

void tick(gma::Dispatcher& d, const char* symbol,
          std::initializer_list<std::pair<const char*, double>> fields) {
  auto payload = std::make_shared<rapidjson::Document>();
  payload->SetObject();
  auto& al = payload->GetAllocator();
  for (const auto& [name, value] : fields)
    payload->AddMember(rapidjson::StringRef(name), value, al);
  gma::Event ev;
  ev.symbol  = symbol;
  ev.payload = payload;          // ev.type defaults to "tick" — the production type
  d.onTick(ev);
}

// ─── Encoding ──────────────────────────────────────────────────────────────
// Corpus 86 (one symbol, two fields). Tick n carries
//
//     bid = 1000.00 + n        ask = 1000.00 + n + 0.02
//
// so the truth for every tick is a +0.02 spread, and every value decodes
// EXACTLY (not within a tolerance — n is integral and 0.02 is the only
// fraction) back to its (tick, side). Base 1000 keeps every raw price an order
// of magnitude above any difference of two of them, so a raw price can never
// be confused with a `Worker{fn:"diff"}` output.
constexpr double kBase   = 1000.00;
constexpr double kSpread = 0.02;

enum class Side { Bid, Ask, Unknown };

struct Decoded {
  long long tick{-1};
  Side      side{Side::Unknown};
};

Decoded decodeBidAsk(double v) {
  const long long m = std::llround((v - kBase) * 100.0);
  if (m < 0) return {};
  Decoded d;
  d.tick = m / 100;
  switch (m % 100) {
    case 0: d.side = Side::Bid; break;
    case 2: d.side = Side::Ask; break;
    default: return {};
  }
  return d;
}

const char* sideName(Side s) {
  switch (s) {
    case Side::Bid: return "bid";
    case Side::Ask: return "ask";
    default:        return "??";
  }
}

// How a recovered tuple is wrong. A tuple is CORRECT only if it is
// {ask(n), bid(n)} in that order — the order a pairwise `diff` needs to yield
// +0.02.
enum class Kind { Correct, SignFlip, SameSide, CrossTick, Undecodable };

Kind classifyTuple(double first, double second) {
  const Decoded a = decodeBidAsk(first), b = decodeBidAsk(second);
  if (a.side == Side::Unknown || b.side == Side::Unknown) return Kind::Undecodable;
  if (a.side == b.side)  return Kind::SameSide;    // ask-with-ask -> a +-k.00 spread
  if (a.tick != b.tick)  return Kind::CrossTick;
  return (a.side == Side::Ask) ? Kind::Correct : Kind::SignFlip;
}

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Correct:     return "CORRECT";
    case Kind::SignFlip:    return "SIGN_FLIP";
    case Kind::SameSide:    return "SAME_SIDE";
    case Kind::CrossTick:   return "CROSS_TICK";
    default:                return "UNDECODABLE";
  }
}

struct TupleReport {
  std::size_t tuples{0};
  std::size_t wrong{0};
  std::size_t signFlip{0};
  std::size_t sameSide{0};
  std::size_t crossTick{0};
  std::size_t undecodable{0};
  std::size_t oddRuns{0};        // per-thread runs of odd length: pairing unsound
  std::string examples;          // first few offending tuples, rendered
};

void accumulate(const Drive& d, TupleReport& r, std::size_t maxExamples = 6) {
  for (const auto& [tid, vals] : d.byThread) {
    (void)tid;
    if (vals.size() % 2 != 0) ++r.oddRuns;
    for (std::size_t i = 0; i + 1 < vals.size(); i += 2) {
      ++r.tuples;
      const Kind k = classifyTuple(vals[i], vals[i + 1]);
      if (k == Kind::Correct) continue;
      ++r.wrong;
      switch (k) {
        case Kind::SignFlip:  ++r.signFlip;  break;
        case Kind::SameSide:  ++r.sameSide;  break;
        case Kind::CrossTick: ++r.crossTick; break;
        default:              ++r.undecodable; break;
      }
      if (r.wrong <= maxExamples) {
        const Decoded a = decodeBidAsk(vals[i]), b = decodeBidAsk(vals[i + 1]);
        std::ostringstream os;
        os << "\n      " << kindName(k) << ": {" << std::fixed
           << std::setprecision(2) << vals[i] << ", " << vals[i + 1]
           << "}  = {" << sideName(a.side) << "(tick " << a.tick << "), "
           << sideName(b.side) << "(tick " << b.tick << ")}"
           << "  -> pairwise diff " << std::showpos << (vals[i] - vals[i + 1])
           << std::noshowpos << " (truth: +" << kSpread << ")";
        r.examples += os.str();
      }
    }
  }
}

// ─── Budget ────────────────────────────────────────────────────────────────
constexpr unsigned    kRaceThreads = 4;     // the worst width the probe measured
constexpr std::size_t kRaceTicks   = 1000;
constexpr std::size_t kRaceReps    = 4;     // 4000 tuples; see the budget note above

} // namespace corpus_values

// ═══ 1. Defect 2 — the join counts values, not inputs ══════════════════════
//
// DETERMINISTIC, at threads=1, and red today for a reason that has nothing to
// do with scheduling.
//
// Corpus 86 declares a two-input join: Listener(AAPL,ask) and
// Listener(AAPL,bid). Drive ticks that carry ONLY `ask`. The `bid` input never
// fires once. A two-input join cannot complete a single tuple, so the terminal
// must receive nothing.
//
// Today `Aggregate` buffers per `sv.symbol` with no input index and fires at
// `vals.size() >= arity_`, so eight `ask` values become four "complete tuples"
// of {ask(n), ask(n+1)} — a pairwise spread of exactly -1.00 where the truth
// is 0.02. That is SPEC §0's headline number, reproduced here with no race,
// no repetition and no thread-count dependence.
//
// Turns green with ENC-1291 (port-indexed fan-in + arity enforcement, D2).
TEST(CorpusValueAssertions, Corpus86_JoinMustNotCompleteFromOneInputAlone) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";

  constexpr std::size_t kTicks = 8;

  Drive d = driveCorpus(*req, /*threads=*/1, [](gma::Dispatcher& disp) {
    for (std::size_t n = 0; n < kTicks; ++n)
      tick(disp, "AAPL", {{"ask", kBase + double(n) + kSpread}});  // no "bid"
  });

  TupleReport r;
  accumulate(d, r);

  EXPECT_EQ(d.arrivals, 0u)
      << "corpus_id 86 \"Bid-ask spread for AAPL\" declares a TWO-input join "
         "(Listener(AAPL,ask) + Listener(AAPL,bid)).\n"
         "    " << kTicks << " ticks carrying ONLY `ask` were injected; `bid` never "
         "fired.\n"
         "    A two-input join has no complete tuple to emit, so the terminal must "
         "see 0 values.\n"
         "    It saw " << d.arrivals << ", forming " << r.tuples << " \"tuple(s)\", "
      << r.wrong << " of them wrong (" << r.sameSide << " same-side):"
      << r.examples
      << "\n\n    Cause: src/nodes/Aggregate.cpp:30-32 buffers per `sv.symbol` with no "
         "input index and\n"
         "    fires on `vals.size() >= arity_` — it counts VALUES, not distinct "
         "INPUTS.\n"
         "    Expected green after ENC-1291 (port-indexed fan-in + arity "
         "enforcement, SPEC D2).";

  EXPECT_EQ(r.oddRuns, 0u)
      << "per-thread arrival run had odd length — tuple recovery is unsound here";
  EXPECT_EQ(d.nonNumeric, 0u) << "terminal received a non-numeric value";
}

// ═══ 2a. Defect 4 — CONTROL. The same checker, at one thread. ══════════════
//
// PASSES today, and must keep passing.
//
// This test exists to falsify the checker, not the engine. At threads=1 the
// join is correct by construction (FIFO queue, one worker, `std::map` field
// order puts `ask` before `bid`), so a green here proves `classifyTuple` can
// return CORRECT at all — without which the red in 2b below would prove
// nothing. It also pins, in the suite itself, the reason a single-threaded
// value assertion is not a gate for defect 4.
TEST(CorpusValueAssertions, Corpus86_SpreadIsExactlyTwoCents_SingleThreadControl) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";

  Drive d = driveCorpus(*req, /*threads=*/1, [](gma::Dispatcher& disp) {
    for (std::size_t n = 0; n < kRaceTicks; ++n)
      tick(disp, "AAPL", {{"bid", kBase + double(n)},
                          {"ask", kBase + double(n) + kSpread}});
  });

  TupleReport r;
  accumulate(d, r);

  ASSERT_EQ(r.oddRuns, 0u) << "tuple recovery unsound (odd-length per-thread run)";
  ASSERT_GT(r.tuples, 0u) << "the join emitted nothing — the control proves nothing";
  EXPECT_EQ(r.wrong, 0u)
      << "CONTROL FAILED. At threads=1 this join is correct BY CONSTRUCTION and the\n"
         "    committed probe (specs/2026-09-20-gma-join-correctness/probe/) measured 0\n"
         "    wrong tuples in 1,200,000 consecutive single-threaded tuples. A failure\n"
         "    here means either the engine's single-threaded ordering changed or the\n"
         "    checker below is broken — investigate before trusting any verdict from\n"
         "    Corpus86_SpreadIsExactlyTwoCents."
      << r.examples;
}

// ═══ 2b. Defect 4 — the value assertion. THE spread must be 0.02. ═════════
//
// RED today. This is the "emitted value asserted exactly" the ticket asks for:
// every tuple the join completes must be {ask(n), bid(n)} of the SAME tick, in
// that order, so a pairwise diff is exactly +0.02 — never +-1.00 (ask paired
// with ask), never -0.02 (sign flip), never +-0.98 (across ticks).
//
// Runs at threads=4 for `kRaceReps` x `kRaceTicks` ticks. See the budget note
// at the top of this block for the probability this passes by luck (~2e-30 at
// the probe's most favourable observed rate).
//
// Turns green with ENC-1005 (per-request strand, SPEC D3/D4).
TEST(CorpusValueAssertions, Corpus86_SpreadIsExactlyTwoCents) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";

  TupleReport r;
  for (std::size_t rep = 0; rep < kRaceReps; ++rep) {
    Drive d = driveCorpus(*req, kRaceThreads, [](gma::Dispatcher& disp) {
      for (std::size_t n = 0; n < kRaceTicks; ++n)
        tick(disp, "AAPL", {{"bid", kBase + double(n)},
                            {"ask", kBase + double(n) + kSpread}});
    });
    accumulate(d, r);
  }

  ASSERT_EQ(r.oddRuns, 0u)
      << "tuple recovery unsound (odd-length per-thread run) — discard this verdict";
  ASSERT_GT(r.tuples, 0u) << "the join emitted nothing";

  const double pct = 100.0 * double(r.wrong) / double(r.tuples);
  EXPECT_EQ(r.wrong, 0u)
      << "corpus_id 86 \"Bid-ask spread for AAPL\": the spread is 0.02 on every tick.\n"
         "    Driven at threads=" << kRaceThreads << ", " << kRaceReps << " x "
      << kRaceTicks << " ticks.\n"
         "    " << r.wrong << " of " << r.tuples << " tuples (" << std::fixed
      << std::setprecision(2) << pct << "%) pair the wrong two values:\n"
         "      same-side (ask with ask / bid with bid, spread +-k.00): " << r.sameSide << "\n"
         "      sign flip (bid before ask, spread -0.02):               " << r.signFlip << "\n"
         "      cross-tick:                                             " << r.crossTick << "\n"
      << r.examples
      << "\n\n    Cause: Listener::onValue posts every value to the shared ThreadPool "
         "queue\n"
         "    (src/nodes/Listener.cpp:84), so two ticks from one Listener run "
         "concurrently\n"
         "    and Aggregate's value-counting buffer completes a batch from whichever "
         "two\n"
         "    values land first. At threads=1 this test's control twin passes; only the\n"
         "    pool width differs.\n"
         "    Expected green after ENC-1005 (per-request strand, SPEC D3/D4).";
}

// ═══ 3. Defect 3 — the correlation key is `symbol`, so no cross-symbol join ═
//
// DETERMINISTIC, at threads=1, red today.
//
// Corpus 87 "Price difference between AAPL and MSFT" declares a two-input join
// across two streamKeys. Drive six ticks on each side. Whatever the engine
// emits, every completed tuple must contain one value from each side — that is
// what "join" means, and it is the one statement that holds under BOTH answers
// the SPEC leaves open for D1:
//
//   `by:"streamKey"` (the locked default) -> the two sides never share a key,
//        so the correct emission count is 0 and this assertion holds vacuously;
//   `by:"none"` (the cross-symbol join D1 adds) -> 6 tuples, each one AAPL
//        value and one MSFT value.
//
// Today it is neither: `buf_[sv.symbol]` gives AAPL and MSFT one independent
// buffer each, and each fires every second value, so the terminal receives six
// tuples of {AAPL, AAPL} and {MSFT, MSFT} — a "price difference between AAPL
// and MSFT" computed without ever looking at both.
//
// NOTE the vacuous-pass branch: under the locked default this test can go green
// by the join emitting nothing. That is honest (an inert request is better than
// a wrong answer) but it is weak, and ENC-1292 should strengthen it to the
// `by:"none"` form once `by` exists.
//
// Corpus 87 also carries `pipeline:[Worker{fn:"diff"}]`, which SPEC §1.1
// defect 1 wires as a SECOND live chain into the same terminal. Its values are
// differences of AAPL prices (0..5 here), two orders of magnitude below any raw
// price, so they are filtered out by value rather than assumed away.
TEST(CorpusValueAssertions, Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(87);
  ASSERT_NE(req, nullptr) << "corpus_id 87 not found in corpus_requests.json";

  constexpr double kAaplBase = 1000.0;   // AAPL lastPrice(n) = 1000 + n
  constexpr double kMsftBase = 5000.0;   // MSFT lastPrice(n) = 5000 + n
  constexpr std::size_t kTicksPerSide = 6;

  Drive d = driveCorpus(*req, /*threads=*/1, [](gma::Dispatcher& disp) {
    for (std::size_t n = 0; n < kTicksPerSide; ++n) {
      tick(disp, "AAPL", {{"lastPrice", kAaplBase + double(n)}});
      tick(disp, "MSFT", {{"lastPrice", kMsftBase + double(n)}});
    }
  });

  // Keep only raw prices: the second (pipeline) chain emits diffs of AAPL
  // prices, all < 10 here. Aggregate forwards a batch back-to-back from one
  // thread, so dropping the interlopers preserves batch adjacency.
  std::vector<double> joined;
  for (const auto& [tid, vals] : d.byThread) {
    (void)tid;
    for (double v : vals) if (v >= 900.0) joined.push_back(v);
  }

  auto sym = [&](double v) { return v >= kMsftBase ? 'M' : 'A'; };

  std::size_t sameSide = 0;
  std::ostringstream examples;
  ASSERT_EQ(joined.size() % 2, 0u)
      << "odd number of join arrivals (" << joined.size() << ") — recovery unsound";
  for (std::size_t i = 0; i + 1 < joined.size(); i += 2) {
    if (sym(joined[i]) != sym(joined[i + 1])) continue;
    ++sameSide;
    if (sameSide <= 6)
      examples << "\n      {" << std::fixed << std::setprecision(2) << joined[i]
               << ", " << joined[i + 1] << "}  = both " << sym(joined[i])
               << "  -> \"AAPL minus MSFT\" = "
               << std::showpos << (joined[i] - joined[i + 1]) << std::noshowpos;
  }

  EXPECT_EQ(sameSide, 0u)
      << "corpus_id 87 \"Price difference between AAPL and MSFT\" declares a join "
         "across TWO\n"
         "    streamKeys. " << kTicksPerSide << " ticks were driven into each side. "
      << (joined.size() / 2) << " tuple(s) were emitted and\n"
         "    " << sameSide << " of them pair one symbol with ITSELF:" << examples.str()
      << "\n\n    Cause: src/nodes/Aggregate.cpp:30 keys the buffer on `sv.symbol`, so "
         "AAPL and\n"
         "    MSFT occupy two independent buffers and each completes alone. "
         "src/nodes/Pack.cpp:32\n"
         "    keys the same way — no cross-streamKey join exists in the engine at all "
         "(SPEC\n"
         "    §1.1 defect 3; 23 of the 52 corpus Aggregate requests ask for one).\n"
         "    Expected green after ENC-1291 (D2); ENC-1292 (declared `by`, D1) should "
         "then\n"
         "    strengthen this to assert 6 mixed tuples rather than 0.";

  EXPECT_EQ(d.nonNumeric, 0u) << "terminal received a non-numeric value";
}
