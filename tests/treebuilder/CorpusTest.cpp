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
// MEASURED, NOT ASSUMED (ENC-1289, GMA_V3@7928d78, g++ 16.2.1, AMD Ryzen 7
// 5800XT 8C/16T, Fedora 44, Release; 1-minute load average 12.4-17.6
// throughout — other agents were compiling)
//
//   Corpus86_JoinMustNotCompleteFromOneInputAlone   red 40/40 runs, and every
//       run reported the SAME numbers (8 arrivals, 4 tuples, 4 same-side).
//   Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself
//                                                   red 40/40 runs, same
//       numbers every run (6 tuples, 6 same-side).
//   Corpus86_SpreadIsExactlyTwoCents                red 40/40 runs; wrong-tuple
//       rate per invocation 3.62% - 8.22% of 4000 tuples (mean ~6.4%),
//       consistent with the probe's pooled 6.62-7.27% at this width. At the
//       lowest rate observed here the chance of a clean pass is (1-0.0362)^4000
//       ~ 1e-64; at the probe's all-time minimum single-run rate (1.70%) it is
//       ~1e-30.
//       ENC-1005 (2026-09-20): GREEN 40/40 after the per-request strand —
//       160,000 further tuples at threads=4, 0 wrong. The rates above are what
//       the same binary measured the day before, and are what makes the green
//       mean something.
//   Corpus86_SpreadIsExactlyTwoCents_SingleThreadControl
//                                                   green 40/40 runs
//       = 40,000 further single-threaded tuples, 0 wrong.
//
// FALSIFIED IN BOTH DIRECTIONS. A check never seen fail is not a check; a check
// that can only fail is not one either. Each assertion was run against a
// throwaway prototype of the fixes it names (NOT committed, reverted after
// measurement):
//
//   port-indexed fan-in alone (a prototype of ENC-1291/D2: one slot per
//       declared input, emit only when all are filled)
//         -> JoinMustNotCompleteFromOneInputAlone            RED  -> GREEN
//         -> Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself
//                                                            RED  -> GREEN
//         -> SpreadIsExactlyTwoCents      still RED, and its failures changed
//            character completely: 0 same-side, 0 sign-flip, 1303/3979 (32.75%)
//            CROSS_TICK. Exactly what SPEC D3 predicts is left once the
//            counting defect is gone, and evidence the classifier discriminates
//            between the two defects rather than reporting one number.
//   + ordered delivery (a prototype of ENC-1005/D3: both ThreadPool hops made
//       synchronous)
//         -> all four GREEN at threads=4.
//
// ENC-1005 SHIPPED ordered delivery and it flipped EXACTLY ONE of the three, as
// the line above predicts for the ordering defect alone: SpreadIsExactlyTwoCents
// RED -> GREEN, and the two state-machine gates (defects 2 and 3, both driven at
// threads=1) unmoved. The shipped form is not "both hops synchronous": hop one
// (Dispatcher -> Listener) is inline, hop two (Listener -> the DAG) is a
// per-request `rt::Strand`, so the compute is still off the ingress thread and
// the thread count is unchanged.
//
// ───────────────────────────────────────────────────────────────────────────
// THREE OF THESE FAIL TODAY, ON PURPOSE, AND `ctest` IS STILL GREEN
//
// SPEC D8: "an instrument that cannot fail is not a gate. This is sequenced
// first deliberately." So three of the four assertions below are red against
// this engine and are MEANT to be.
//
// They are not disabled, skipped or commented out. Each is registered in
// CMakeLists.txt as its own ctest case with `WILL_FAIL TRUE`: the assertion
// executes on every run, ctest reports the case as a pass *because* the
// assertion fails, and the suite stays green for the other sessions building
// this repo. A red `ctest` on master would have turned "is the build green?"
// into a question everyone has to re-investigate, and the answer decays to
// "ignore those three" within a day.
//
// The property that makes this better than a DISABLED_ test: it unwinds
// itself. When the fix lands the assertion starts passing, `WILL_FAIL` flips
// its ctest case RED, and whoever landed the fix has to come here and remove
// the marker. A disabled test rots silently; this one demands attention
// exactly once, at the moment it becomes wrong.
//
//   Corpus86_JoinMustNotCompleteFromOneInputAlone  -> ENC-1291 (port-indexed
//                                                     fan-in + arity, D2)
//   Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself
//                                                  -> ENC-1291, and ENC-1292
//                                                     (declared `by`, D1)
//
// TWO, not three, as of ENC-1005 (2026-09-20). `Corpus86_SpreadIsExactlyTwoCents`
// was the third; SPEC D3's per-request strand landed, it went green, its
// `WILL_FAIL` block was deleted and the count pinned by
// `gma_enc1289_expected_failures_really_failed` went from 3 to 2. It now runs
// inside the main `gma_tests` case. This paragraph unwinding itself one line at
// a time is the mechanism working as designed.
//
// `Corpus86_SpreadIsExactlyTwoCents_SingleThreadControl` is deliberately NOT
// registered that way. It is an ordinary always-green test — inverting it
// would assert that the checker must never return clean, which is the opposite
// of what a control is for.
//
// To see the real verdicts rather than the inverted ones, run the binary
// directly: `./gma_tests --gtest_filter='CorpusValueAssertions.*'`. That is
// also the form the ENC-1065 `--gtest_repeat` check needs (full, unfiltered).
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

// ─── The join's observation point (ENC-1290) ───────────────────────────────
//
// Every assertion in this block is about the JOIN: which values `Aggregate`
// pairs into a tuple. Until ENC-1290 the join's raw members reached the
// terminal directly, because a request carrying both `node` and `pipeline`
// built TWO live chains into one Responder (SPEC §1.1 defect 1) and the
// `node` chain was one of them. Driving only the join's own input fields left
// the other chain silent, so the terminal stream WAS the join's output.
//
// D5 composed the two into one chain, so for corpus 86 and 87 the terminal now
// sees `Worker{fn:"diff"}` output instead. That is the fix working, and it
// moves the observation point rather than changing what the join does. Left
// alone, these tests would have gone quietly wrong in three different ways —
// all three measured before this change landed:
//
//   * Corpus86_SpreadIsExactlyTwoCents      — HOLLOWED OUT. `decodeBidAsk`
//     decodes 0 of 6 arrivals, every tuple scores UNDECODABLE, and it stays
//     red for a reason ENC-1005 can never clear.
//   * ..._SingleThreadControl               — the same checker, and it is NOT
//     in GMA_ENC1289_EXPECTED_FAILURES, so it runs inside the MAIN `gma_tests`
//     ctest case and took the primary target red.
//   * Corpus87_CrossSymbolJoin...           — BROKEN. Its `>= 900.0` filter
//     (there to drop the second chain's small diffs) kept 0 values, the test
//     passed vacuously, and its WILL_FAIL ctest case flipped to FAILED —
//     taking `gma_enc1289_expected_failures_really_failed` with it.
//
// So each of them drives the corpus entry's `node` WITHOUT its pipeline. The
// request is still built by the real `tree::buildForRequest` and driven
// through the real `Dispatcher`, the `node` is the corpus entry's own
// verbatim, and `tests/treebuilder/corpus_requests.json` is NOT touched. The
// assertions, their thread counts, their tick encodings and their expected
// red/green states are all byte-for-byte what ENC-1289 committed.
//
// What D5 itself introduced — the composed chain end to end, including corpus
// 86's exact emitted values through `Worker{fn:"diff"}` — is asserted in
// tests/treebuilder/ComposedChainTest.cpp, not here.
rapidjson::Document nodeWithoutPipeline(const rapidjson::Value& request) {
  rapidjson::Document d;
  d.CopyFrom(request, d.GetAllocator());
  d.RemoveMember("pipeline");
  d.RemoveMember("stages");
  return d;
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
//
// +-- EXPECTED TO FAIL ----------------------------------------------------+
// | Registered in CMakeLists.txt as ctest case                             |
// |   gma_enc1289_xfail_join_counts_values_not_inputs   WILL_FAIL TRUE     |
// | WHEN ENC-1291 MAKES THIS PASS, that ctest case goes RED. Delete its    |
// | add_test/set_tests_properties block and drop this test's name from the |
// | GMA_ENC1289_EXPECTED_FAILURES list. Do not touch the assertion.        |
// +------------------------------------------------------------------------+
TEST(CorpusValueAssertions, Corpus86_JoinMustNotCompleteFromOneInputAlone) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";
  rapidjson::Document join = nodeWithoutPipeline(*req);   // ENC-1290, see above

  constexpr std::size_t kTicks = 8;

  Drive d = driveCorpus(join, /*threads=*/1, [](gma::Dispatcher& disp) {
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
//
// NOT registered WILL_FAIL, deliberately: this one is an ordinary always-green
// ctest case. Inverting it would assert that the checker must never return
// clean, which is the opposite of what a control is for.
TEST(CorpusValueAssertions, Corpus86_SpreadIsExactlyTwoCents_SingleThreadControl) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";
  rapidjson::Document join = nodeWithoutPipeline(*req);   // ENC-1290, see above

  Drive d = driveCorpus(join, /*threads=*/1, [](gma::Dispatcher& disp) {
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
// GREEN as of ENC-1005 (per-request strand, SPEC D3/D4), 2026-09-20. This is
// the "emitted value asserted exactly" ENC-1289 was asked for: every tuple the
// join completes must be {ask(n), bid(n)} of the SAME tick, in that order, so a
// pairwise diff is exactly +0.02 — never +-1.00 (ask paired with ask), never
// -0.02 (sign flip), never +-0.98 (across ticks).
//
// Runs at threads=4 for `kRaceReps` x `kRaceTicks` ticks. See the budget note
// at the top of this block for the probability this would pass by luck if the
// defect were still present (~2e-30 at the probe's most favourable observed
// rate) — which is what makes it a gate rather than a sample.
//
// +-- WAS `WILL_FAIL`, AND IS NOT ANY MORE -------------------------------+
// | ENC-1289 registered this as ctest case                                |
// |   gma_enc1289_xfail_spread_is_not_two_cents_under_concurrency         |
// | with WILL_FAIL TRUE. ENC-1005 deleted that block and dropped the name |
// | from GMA_ENC1289_EXPECTED_FAILURES, so this test now runs inside the  |
// | main `gma_tests` ctest case like any ordinary test, and a regression  |
// | in ordered delivery takes the PRIMARY target red.                     |
// | The assertion is byte-for-byte ENC-1289's. Nothing about what it      |
// | checks, how it drives, or how many threads it uses was touched.       |
// +------------------------------------------------------------------------+
//
// MEASURED AFTER THE FIX (ENC-1005, Release, Ryzen 7 5800XT, 1-min load
// average 4.3-6.9): 40 consecutive invocations green = 160,000 tuples at
// threads=4, 0 wrong. Before the fix the same binary's own baseline was red
// 40/40. See the ticket for the full before/after.
TEST(CorpusValueAssertions, Corpus86_SpreadIsExactlyTwoCents) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(86);
  ASSERT_NE(req, nullptr) << "corpus_id 86 not found in corpus_requests.json";

  rapidjson::Document join = nodeWithoutPipeline(*req);   // ENC-1290, see above

  TupleReport r;
  for (std::size_t rep = 0; rep < kRaceReps; ++rep) {
    Drive d = driveCorpus(join, kRaceThreads, [](gma::Dispatcher& disp) {
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
      << "\n\n    THIS IS A REGRESSION IN ORDERED DELIVERY (ENC-1005, SPEC D3/D4).\n"
         "    It passed at 4 threads, 40 consecutive invocations, when ENC-1005 landed.\n"
         "    Ordered delivery is TWO cooperating pieces and breaking either one\n"
         "    reddens this test — check both before looking anywhere else:\n"
         "      1. every Listener of ONE request DAG shares ONE rt::Strand\n"
         "         (tree::Deps::strand, minted per subscription in\n"
         "         ClientSession::handleSubscribe and per DAG in buildForRequest);\n"
         "      2. Dispatcher delivers INLINE to a strand-bearing Listener\n"
         "         (Dispatcher::deliver / INode::deliversOnOwnExecutor) instead of\n"
         "         posting each notification as an independent pool task, which\n"
         "         scrambles ask-vs-bid one hop UPSTREAM of the strand.\n"
         "    Without (2), (1) alone leaves this test red — measured, see ENC-1005.\n"
         "    At threads=1 this test's control twin passes; only the pool width differs.";
}

// ═══ 2c. ENC-1290 — the CONTROL on corpus 87's observation point ═══════════
//
// PASSES today, and must keep passing. NOT registered WILL_FAIL.
//
// Gate 3 below carries a `>= 900.0` filter and an assertion that the filter
// dropped nothing. That assertion cannot gate anything from where it sits:
// gate 3 is registered `WILL_FAIL TRUE`, so it is *required* to fail and ctest
// reads a second failure inside it as the expected one. Injecting the exact
// regression it guards against (stop stripping the pipeline, so the terminal
// sees `Worker{fn:"diff"}` output and the filter keeps 0 of 12 arrivals) leaves
// `gma_enc1289_xfail_no_cross_symbol_join` reporting **Passed**.
//
// Corpus 86 already has such a control (2a, and it is what caught D5 hollowing
// out gate 2). Corpus 87 had none. This is it: the same drive as gate 3, with
// only the soundness of the observation point asserted and no claim about the
// join's correctness — so it is green now, stays green through ENC-1291 and
// ENC-1292, and goes red the moment gate 3 starts measuring the wrong stream.
TEST(CorpusValueAssertions, Corpus87_ObservationPointIsSound_Control) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(87);
  ASSERT_NE(req, nullptr) << "corpus_id 87 not found in corpus_requests.json";
  rapidjson::Document join = nodeWithoutPipeline(*req);

  constexpr double kAaplBase = 1000.0, kMsftBase = 5000.0;
  constexpr std::size_t kTicksPerSide = 6;

  Drive d = driveCorpus(join, /*threads=*/1, [](gma::Dispatcher& disp) {
    for (std::size_t n = 0; n < kTicksPerSide; ++n) {
      tick(disp, "AAPL", {{"lastPrice", kAaplBase + double(n)}});
      tick(disp, "MSFT", {{"lastPrice", kMsftBase + double(n)}});
    }
  });

  std::size_t raw = 0;
  for (const auto& [tid, vals] : d.byThread) {
    (void)tid;
    for (double v : vals) if (v >= 900.0) ++raw;
  }

  EXPECT_EQ(d.nonNumeric, 0u) << "terminal received a non-numeric value";
  EXPECT_EQ(raw, d.arrivals)
      << "CONTROL FAILED. " << (d.arrivals - raw) << " of " << d.arrivals
      << " arrivals are not raw AAPL/MSFT prices.\n"
         "    Corpus 87's `node` is driven WITHOUT its pipeline (see the "
         "ENC-1290 note above the driver),\n"
         "    so every value reaching the terminal is a join member and the "
         "`>= 900.0` filter in gate 3\n"
         "    must drop nothing. If computed values are arriving, gate 3 is "
         "measuring the wrong stream\n"
         "    and will pass VACUOUSLY — which its own WILL_FAIL registration "
         "cannot tell you.";
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
// Corpus 87 also carries `pipeline:[Worker{fn:"diff"}]`. SPEC §1.1 defect 1
// wired that as a SECOND live chain into the same terminal, and the `>= 900.0`
// filter below existed to drop its values. ENC-1290/D5 composed the chains, so
// this test now drives corpus 87's `node` alone (see the ENC-1290 note above
// the driver) and there is no second chain left to filter — which the
// assertion next to the filter now checks rather than assumes.
// +-- EXPECTED TO FAIL ----------------------------------------------------+
// | Registered in CMakeLists.txt as ctest case                             |
// |   gma_enc1289_xfail_no_cross_symbol_join            WILL_FAIL TRUE     |
// | WHEN ENC-1291 MAKES THIS PASS, that ctest case goes RED. Delete its    |
// | add_test/set_tests_properties block and drop this test's name from the |
// | GMA_ENC1289_EXPECTED_FAILURES list. ENC-1292 should then strengthen    |
// | the assertion itself (see the note above about the vacuous pass).      |
// +------------------------------------------------------------------------+
TEST(CorpusValueAssertions, Corpus87_CrossSymbolJoinNeverPairsOneSideWithItself) {
  using namespace corpus_values;

  const rapidjson::Value* req = corpusRequest(87);
  ASSERT_NE(req, nullptr) << "corpus_id 87 not found in corpus_requests.json";
  rapidjson::Document join = nodeWithoutPipeline(*req);   // ENC-1290, see above

  constexpr double kAaplBase = 1000.0;   // AAPL lastPrice(n) = 1000 + n
  constexpr double kMsftBase = 5000.0;   // MSFT lastPrice(n) = 5000 + n
  constexpr std::size_t kTicksPerSide = 6;

  Drive d = driveCorpus(join, /*threads=*/1, [](gma::Dispatcher& disp) {
    for (std::size_t n = 0; n < kTicksPerSide; ++n) {
      tick(disp, "AAPL", {{"lastPrice", kAaplBase + double(n)}});
      tick(disp, "MSFT", {{"lastPrice", kMsftBase + double(n)}});
    }
  });

  // Keep only raw prices. This filter was written to drop the SECOND chain's
  // small diffs; since ENC-1290 composed the chains and this test drives the
  // `node` alone, there is no second chain and nothing to drop. The filter
  // stays — and the assertion below is new — because a filter that silently
  // starts dropping everything is exactly how this test passes VACUOUSLY,
  // which is what D5 would have done to it. It must now say so out loud.
  std::vector<double> joined;
  for (const auto& [tid, vals] : d.byThread) {
    (void)tid;
    for (double v : vals) if (v >= 900.0) joined.push_back(v);
  }
  ASSERT_EQ(joined.size(), d.arrivals)
      << "the >= 900.0 filter dropped " << (d.arrivals - joined.size())
      << " of " << d.arrivals << " arrivals. Driving corpus 87's `node` alone, "
         "every arrival is a raw AAPL/MSFT price and NOTHING should be "
         "filtered. If the terminal is seeing computed values again, this test "
         "is measuring the wrong thing and any verdict below is worthless — "
         "see the ENC-1290 note at the top of this block.";

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
