// Empirical proof of the ENC-101 canonical-pattern fix.
//
// The original ENC-101 PoC subscribed to `ob.best.bid.price` /
// `ob.best.ask.price` / `ob.spread` for NEXO+VALT against
// feed-sim.v3m.xyz and received **0 updates** in 15s — the
// dispatcher-side push path is not wired by `ob::Provider`. Bare keys
// (`lastPrice`, `sma_5`) on the same connection received 170 updates.
//
// Phase 1 of this proposal made the silent failure loud (Listener
// factory rejects `ob.*`); this test demonstrates the canonical
// pattern that REPLACES the broken topology and confirms it produces
// the expected ≥1 update — without needing live feed-sim. The book
// state is supplied directly via `AtomicStore::set`, mimicking what
// `ob::Provider` writes on every book update; the trade-event push is
// supplied via `Dispatcher::notifyListeners`, mimicking what
// `MarketTA.cpp:400` calls on every trade tick.
//
// Closes AC #6 of the SPEC at
// gma_v3/specs/2026-05-06-ob-keys-pipeline-only/.

#include "gma/AtomicStore.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/StreamValue.hpp"
#include "gma/nodes/AtomicAccessor.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/nodes/Listener.hpp"
#include "gma/rt/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

using namespace gma;

namespace {

// Captures values emitted by the canonical-pattern terminal. Mirrors
// the WS Responder's role in production.
class CapturingTerminal : public INode {
public:
    std::mutex mx;
    std::vector<StreamValue> received;
    std::atomic<int> count{0};
    void onValue(const StreamValue& sv) override {
        std::lock_guard<std::mutex> lk(mx);
        received.push_back(sv);
        count.fetch_add(1, std::memory_order_release);
    }
    void shutdown() noexcept override {}
    size_t size() {
        std::lock_guard<std::mutex> lk(mx);
        return received.size();
    }
};

} // namespace

// Drives the canonical pattern end-to-end:
//   Listener("NEXO", "lastPrice")  →  AtomicAccessor("NEXO", "ob.best.bid.price")
//                                    →  CapturingTerminal
//
// Three dispatcher.notifyListeners calls (each simulates a trade event)
// produce three terminal hits, each carrying the latest
// `ob.best.bid.price` from the store.
TEST(Enc101CanonicalPatternTest, ListenerClockPlusAtomicAccessorEmitsObValues) {
    rt::ThreadPool pool(1);
    AtomicStore    store;
    Dispatcher     dispatcher(&pool, &store);

    // Pre-populate the store as ob::Provider would on book updates.
    store.set("NEXO", "ob.best.bid.price", 24.83);

    auto terminal = std::make_shared<CapturingTerminal>();
    auto accessor = std::make_shared<AtomicAccessor>(
        "NEXO", "ob.best.bid.price", &store, terminal);

    // Build the Listener via the production factory — same path the
    // TreeBuilder uses. This also exercises the ENC-101 reject for
    // `ob.*` (covered separately in ListenerTest); here it accepts
    // the bare-key clock.
    auto listenerRes = nodes::Listener::Create(
        "NEXO", "lastPrice", accessor, &pool, &dispatcher);
    ASSERT_TRUE(listenerRes.has_value())
        << "factory must accept bare-key clock 'lastPrice'";

    // Three "trade events" in sequence — what MarketTA.cpp:400 emits
    // for every NEXO trade. The values are discarded by
    // AtomicAccessor; only the trigger matters.
    constexpr int kEvents = 3;
    for (int i = 0; i < kEvents; ++i) {
        dispatcher.notifyListeners("NEXO", "lastPrice", 100.0 + i);
    }

    // Drain the threadpool so the AtomicAccessor's reads + terminal
    // writes complete deterministically. (Each push goes through
    // pool tasks twice — Listener forward + AtomicAccessor read —
    // so we cannot reason about ordering without a drain.)
    pool.shutdown();

    // The AC #6 claim: ≥1 update where ENC-101's repro saw 0.
    EXPECT_GE(terminal->size(), 1u)
        << "ENC-101 PoC saw 0 updates in 15s. The canonical pattern "
           "must produce at least one. Received: " << terminal->size();

    // Tighter: every trade-event trigger reaches the terminal with
    // some value of ob.best.bid.price (book state is whatever the
    // store held at the moment of read; race between the inline
    // store.set and the pool's read tasks can collapse all reads to
    // the latest value, which is fine — AC #6 is "non-zero", not
    // "every value preserved").
    EXPECT_EQ(terminal->size(), static_cast<size_t>(kEvents));
    for (const auto& sv : terminal->received) {
        EXPECT_EQ(sv.symbol, "NEXO");
        // AtomicAccessor only forwards when the store holds a value.
        // If the canonical pattern were broken, the value here would
        // be missing; instead it's a sane double from the store.
        const double v = std::get<double>(sv.value);
        EXPECT_GT(v, 0.0) << "expected ob.best.bid.price > 0; got " << v;
    }
}

// Companion guard: the OLD anti-pattern — Listener bound directly to
// `ob.best.bid.price` — fails at construct time. This is the dual of
// the canonical-pattern test: it asserts the canonical pattern is the
// ONLY way to surface ob.* into a chart.
TEST(Enc101CanonicalPatternTest, ListenerOnObFailsAtConstructTime) {
    rt::ThreadPool pool(1);
    AtomicStore    store;
    Dispatcher     dispatcher(&pool, &store);

    auto terminal = std::make_shared<CapturingTerminal>();

    auto bad = nodes::Listener::Create(
        "NEXO", "ob.best.bid.price", terminal, &pool, &dispatcher);
    ASSERT_FALSE(bad.has_value())
        << "ENC-101: Listener-on-ob.* must be rejected by the factory";
    EXPECT_NE(bad.error().message.find("pipeline-only"), std::string::npos);

    pool.shutdown();
}
