# GMA_V3 — Deep Code Review (2026-06-26)

Full-repository adversarial review. The entire production tree (`src/`, `include/`,
`connectors/`), the build system, the ~9.8k-line test suite, and all docs/specs were read
and validated. Work was split across five parallel review streams:

1. Engine Core & Runtime  — `src/core`, `src/rt`, `src/util`, engine headers
2. Nodes, Server, WS & Composition — `src/nodes`, `src/server`, `src/ws`, `src/forum`, `main.cpp`
3. Connectors — `connectors/market/**`, `connectors/synthetic/**`
4. Build & Validation — actual build + `ctest`, warnings, CMake/Docker/tooling
5. Test-quality & Docs — test suite soundness + doc/code consistency

**Validation rule:** every finding below was traced to specific code and quoted by the
reviewing stream. Items are marked **CONFIRMED** (traced, real in current code) or
**SUSPECTED** (real code smell whose impact depends on a runtime condition). Findings
flagged **[2 streams]** were independently reproduced by two reviewers.

**Baseline health (good news first):** builds clean with g++ 16.1.1 — **0 errors**, and the
engine library is **warning-clean under `-Wall -Wextra -Wpedantic`**. **All 520 tests pass.**
The engine↔connector boundary rule (`gma_engine` must not depend on connector headers) is
genuinely enforced. The problems below are real but sit against an otherwise disciplined
codebase.

---

## CRITICAL

### C1 — `order_executed` double-mutates the order book (book corruption on every execution) — CONFIRMED
`connectors/market/src/feed/ItchAdapter.cpp:150-162`
An ITCH execution emits **both** an explicit OB mutation (`ObDeleteEvent`/`ObUpdateEvent`)
**and** an `ObTradeEvent`. `WsFeedClient::dispatchEvent` (`ws/WsFeedClient.cpp:341-360`)
routes the update/delete to `onUpdate`/`onDelete` **and** the trade to
`onTrade → OrderBook::applyTrade → consumeAtLevel` (`book/OrderBook.cpp:259-283`), which
decrements the level a second time. Partial fill: the resting order is reduced by
`2×execShares`. Full fill: after the delete, the trade wrongly consumes `execShares` from
*other* orders resting at the same price. Net: every execution against a level with depth
silently corrupts sizes and top-of-book. This is the single most damaging correctness bug
found.
**Fix:** apply only the explicit add/update/delete from an execution; treat the trade as a
TA/print event that does not consume the book.

---

## HIGH

### H1 — `ThreadPool::drain()` lost-wakeup can hang shutdown forever — CONFIRMED **[2 streams]**
`src/rt/ThreadPool.cpp:33-36` (drain wait), `:67-68` (notify)
The worker decrements `inFlight_` and calls `idleCv_.notify_all()` **without holding `mx_`**,
while `drain()` waits under `mx_` on the predicate `q_.empty() && inFlight_==0`. If the final
in-flight task completes in the window after `drain()` evaluates the predicate and before it
parks, the notification is lost and there is no subsequent notify — `drain()` blocks forever.
`drain()` is the first step of `shutdown()` and is wired into the shutdown coordinator
(`main.cpp:116`), so this can wedge process exit.
**Fix:** decrement `inFlight_` and/or call `notify_all()` while holding `mx_`.

### H2 — Subscription teardown leaks the keep-alive chain → timer-node threads leak forever — CONFIRMED
`src/server/ClientSession.cpp:159-168` (close), `:487-496` (subscribe-replace), `:563-575` (cancel)
`ClientSession` tracks `active_` (head node) and `chains_` (every node, the `keepAlive`
vector). On all three teardown paths it shuts down only the **head** and clears/overwrites
`chains_` **without calling `shutdown()` on the chain**. Head `shutdown()` does not propagate
down a linear pipeline (`Listener`/`Worker`/`Expr`/`Filter`/`Field`/`AtomicAccessor`/
`VectorReducer`/`Aggregate::shutdown` only reset their own downstream). A mid-pipeline
`TumblingWindow`/`Interval`/`BucketTime` therefore never has `shutdown()` called: its timer
thread captures `shared_from_this()` (`src/nodes/TumblingWindow.cpp:55`), so the refcount
never drops, `stopping_` is never set, and the timer keeps firing forever into a dead
pipeline. One leaked OS thread + pipeline per cancelled/closed time-bucketed subscription.
The (test-only) `WsBridge` does this correctly (`src/ws/WsBridge.cpp:267-271, 309-315`) —
`ClientSession` is missing exactly that loop.
**Fix:** in all three paths, iterate the removed `chains_[key]` vector and call
`node->shutdown()` on each, mirroring `WsBridge`.

### H3 — Two different classes share the FQN `gma::util::MetricRegistry` — ODR violation (UB) — CONFIRMED
`include/gma/Metrics.hpp:20-123` vs `include/gma/util/Metrics.hpp:24-49`
Both headers define `gma::util::MetricRegistry` with **completely different** members and
APIs. `src/util/Metrics.cpp` compiles the first; `main.cpp` includes only the second and
calls `MetricRegistry::instance().startReporter(...)`. Two distinct definitions with the same
mangled name in one binary is a textbook ODR violation (undefined behavior). It "works" today
only because the three overlapping method names coincide — `main.cpp` ends up calling method
bodies compiled against a *different* object layout. The util header's non-overlapping API
(`counter()/gauge()/snapshotJson()`) has no definition anywhere.
**Fix:** collapse to one `MetricRegistry` in one header, or rename one (e.g.
`gma::obmetrics::MetricRegistry`).

### H4 — Live `ob::Provider` resolves only a fraction of the advertised `ob.*` key grammar — CONFIRMED **[2 streams]**
`connectors/market/src/MarketConnector.cpp:156-159` wires `ob::Provider::get`;
`connectors/market/src/ob/ObProvider.cpp:32-90`
The production resolver handles only `ob.spread`, `ob.level.*`, `ob.cum.*`, and
`ob.imbalance.levels.*` — **everything else returns NaN**, including basic TOB keys the
catalog advertises (`ob.best.bid.price`, `ob.best.ask.size`, `ob.mid`, `ob.vwap.*`,
`ob.meta.*` — see `ObKeysCatalog.hpp:7-19`). The full evaluator that *does* handle these
(`ObMaterializer::eval` + `parseObKey`) is `#include`d but **never invoked in production**
(confirmed: only callers are `ObKey.cpp` and the tests). So a large, exhaustively-tested body
of order-book key code is dead, and the documented keys silently resolve to NaN.
**Fix:** route `getImpl` through `parseObKey`+`eval`, or extend it to cover best/mid/vwap/
range/at/meta.

### H5 (build) — `-march=native` baked into the shipped Docker image — CONFIRMED
`CMakeLists.txt:211-212` adds `-O3 -march=native` for Release; the `Dockerfile` builds
Release. The container's `gma_server` is compiled for the **build machine's exact CPU** and
can `SIGILL` on a different/older deploy host or CI builder.
**Fix:** use `-march=x86-64-v2/v3`, or make `-march=native` opt-in for non-portable artifacts.

### H6 (build) — No `.dockerignore`; host `build/` (with absolute-path `CMakeCache.txt`) leaks into image build — CONFIRMED
The Dockerfile `COPY GMA_V3/ ...` then `cmake ..`; with no `.dockerignore`, the host `build/`
dir is copied in and the stale cache breaks the Docker build non-deterministically.
**Fix:** add `.dockerignore` excluding `build*/` (and `third_party` as appropriate).

---

## MEDIUM

### M1 — ITCH `trade` prints mutate the displayed book — CONFIRMED
`connectors/market/src/feed/ItchAdapter.cpp:276` (`routeTrade`)
A top-level ITCH `"trade"` message emits `ObTradeEvent`, which consumes resting orders via
`applyTrade`. NASDAQ ITCH `Trade` messages are non-displayable/hidden executions and must not
move the displayed book; with `Aggressor::Unknown` the passive side is guessed from TOB
(`OrderBook.cpp:71-74`), so a hidden print at the touch consumes genuine displayed liquidity.
**Fix:** trade prints should produce only the TA tick, not a book consume.

### M2 — `ItchAdapter` validates `IsNumber()` but calls `GetUint64()`/`GetInt()` — feed-driven DoS / garbage reads — CONFIRMED
`connectors/market/src/feed/ItchAdapter.cpp` (e.g. `:115-116, 138-139, 175-176, 223-225`;
`stockLocate` `GetInt()` at `:74, 108, 259`)
`IsNumber()` is true for doubles/negatives, but RapidJSON's `GetInt()`/`GetUint64()` assert on
the exact type flag. A malformed frame (`"orderRef":1.5`, `"shares":-3`) → `abort()` in
Debug/test builds (feed-controlled DoS) and a raw-union garbage read under `NDEBUG` (poisoned
order ids). `FeedServer.cpp:223-258` does this correctly with `IsUint64()`; ItchAdapter is the
inconsistent path.
**Fix:** gate with `IsUint64()`/`IsInt()` before the typed getter.

### M3 — `maxSymbols` cap is bypassed for bid/ask/spread/timestamp atomics — CONFIRMED
`connectors/market/src/MarketTA.cpp:348-360`
The four base atomics are written **before** the `_maxSymbols` gate at `:357-360`. A feed
spraying unique symbols is dropped from history/TA but still writes four atomics per new
symbol into `AtomicStore`, growing it unboundedly despite the cap.
**Fix:** apply the symbol-count gate before writing any per-symbol atomics.

### M4 — `ItchAdapter::orders_` grows unbounded — CONFIRMED
`connectors/market/include/gma/feed/ItchAdapter.hpp:55`; `ItchAdapter.cpp:123, 239`
`orders_` is inserted on every add/replace and erased only on delete/full-execute/full-cancel.
A long session, or a feed of orders never subsequently deleted, grows the map without bound
(feed-controlled).
**Fix:** bound with an LRU or periodic prune.

### M5 — `FeedServer::stop()`/`FeedSession::close()` touch Asio socket/timer off-strand — CONFIRMED
`connectors/market/src/server/FeedServer.cpp:378-393` (stop), `:40-50` (close), `:399` (per-session strand)
`stop()` calls `s->close()` (which does `idleTimer_.cancel()`, `socket_.shutdown/close()`)
from the caller's thread while the session's strand handlers may be in flight — a data race on
non-thread-safe Asio objects.
**Fix:** `boost::asio::post(strand, [self]{ self->close(); })`.

### M6 — `OrderBookManager::mutateWithDelta_` before/mutate/after sequence is not atomic under concurrent same-symbol mutation — CONFIRMED (weak test) / SUSPECTED (race)
`connectors/market/src/book/OrderBookManager.cpp:161-211`
Each `OrderBook` call locks independently; no manager-level lock spans the
pre-TOB → mutate → post-TOB probe. `FeedServer` accepts each producer on its own strand, so
two connections mutating the same symbol can interleave: the book stays internally consistent
but the emitted `LevelDelta`/TOB deltas and `pubSeq_` ordering (`:153-157`) can be
wrong/non-monotonic. The lone concurrency test uses one symbol and asserts only invariants
(which hold regardless), so it cannot detect this.
**Fix:** hold a per-symbol lock across the whole before/mutate/after window.

### M7 — Sequence/gap handling (`onSeq`/`onReset`) is implemented but never invoked by either ingress — CONFIRMED
`connectors/market/src/book/OrderBookManager.cpp:66-78` (`onSeq`)
Neither `WsFeedClient::dispatchEvent` nor `FeedServer::handleObMessage` ever calls `onSeq`
(both apply with default `FeedScope{}`); only `control/reset` reaches `onReset`. Gap detection,
stale-gating, and snapshot-request logic are dead, so books silently drift on packet loss.
**Fix:** thread a per-message sequence into `onSeq` before applying.

### M8 — Unsynchronized read/write race on `Dispatcher::_computers` — CONFIRMED
`src/core/Dispatcher.cpp:10-12` (write), `:75-79` (read)
`addComputer` does `_computers.push_back(...)` with no lock while `onTick` iterates the vector
with no lock — every other shared structure in `onTick` is locked. If `addComputer` is ever
called after events flow, it is a data race plus a reallocation that invalidates the in-flight
iterator. The method is public with no documented threading contract.
**Fix:** lock it, or document/enforce construction-time-only.

### M9 — Atomic store keyed by `(symbol, fnName)` drops the `field` → cross-field collision — CONFIRMED
`src/core/Dispatcher.cpp:130, 169-199`
`computeAndStoreAtomics` receives `field` but ignores it (`/*field*/`) and stores bare keys
(`mean`, `sum`, `stddev`, …) without a field component. For a symbol with more than one
listened field (e.g. `bid` and `ask`), whichever field ticked last overwrites the others, and
a `Listener` bound to `(symbol,"mean")` fires with the mean of the wrong field.
**Fix:** incorporate `field` into the key, or document/enforce the single-field assumption.

### M10 — Per-tick cost is unbounded in the number of builtins (~50 reducers recomputed every tick) — CONFIRMED
`src/core/Dispatcher.cpp:184-214`
`computeAndStoreAtomics` recomputes **every** registered builtin (~50, several O(N·logN) like
`median`) over the full history and `set`s each one on every field tick — even functions no
one subscribed to. It also copies the whole per-symbol listener map each call (`:180`) and
consumes ~50 of `maxFieldsPerSymbol` with machine-generated atomics. On an HFT feed this
dominates cost and undercuts the bounded-per-tick goal.
**Fix:** compute only subscribed/required functions.

### M11 — Concurrent `onTick` calls `compute()` on shared mutable computer instances — SUSPECTED
`src/core/Dispatcher.cpp:58-71`
The computer cache is filled under `_computerCacheMx`, but `c->compute(tick, ctx)` runs
outside the lock. Two ingress threads (`FeedServer` + `WsFeedClient` both active per
`cfg.ingress[]`) ticking the same event type call `compute()` concurrently on the same cached
instance, which holds per-symbol mutable TA state. Whether this races depends on
`MarketTickComputer`'s internal locking — worth confirming under TSan.

### M12 — `ClientSession` drives one Beast stream on two different strands — CONFIRMED (mismatch) / SUSPECTED (race) — **[2 streams]**
`src/server/ClientSession.cpp:55-56`; `WebSocketServer.cpp:105-106`
The socket is accepted on a per-connection strand (A), but `ClientSession` builds a *second*
strand (B) off the bare `io_context` and binds all user ops to B, while Beast's
`keep_alive_pings`/`idle_timeout` timer runs on A. Two strands provide no mutual exclusion —
a violation of Beast's single-implicit-strand contract. Safe today **only** because
`ioc.run()` is called on exactly one thread (`main.cpp:256`); it becomes a real data race the
moment the io_context is run multi-threaded.
**Fix:** construct `strand_` from `ws_.get_executor()` (reuse strand A), or drop the second
strand.

### M13 — Signal handler performs heavy, non-async-signal-safe shutdown work — CONFIRMED
`src/main.cpp:38-41, 60-65`; `ShutdownCoordinator.hpp:30-58`
`handleSignal` runs the full `stop()` from signal context: locks a mutex, logs (heap/iostreams),
cancels the acceptor, dispatches `close()` to strands, drains/joins the thread pool — none
async-signal-safe (technically UB). Also, single-threaded: the signal interrupts the io
thread, so the `close()` handlers dispatched to strands can't run, and `ioc.stop()` makes
`run()` return before they execute — sessions don't close cleanly on SIGINT/SIGTERM.
**Fix:** signal handler sets an `atomic_flag`/self-pipe/`boost::asio::signal_set`; do real
`stop()` on a normal thread.

### M14 — forum returns empty/no-itch connectors → server boots with zero ingress despite "fail-fast" AC-6 — CONFIRMED
`src/main.cpp:184-205`; `src/forum/ConnectorsClient.cpp:262-291`
The comment claims "do not boot with empty ingress," but `std::exit(1)` only fires on a thrown
exception. `fetchIngresses` returns empty when forum returns `[]` or only non-`itch` rows, so
`cfg.ingress` silently becomes empty and the server boots producing no data — exactly what
AC-6 was meant to prevent.
**Fix:** treat an empty result as fatal (or warn loudly).

### M15 (build) — No sanitizer wiring (ASan/TSan/UBSan) anywhere — CONFIRMED **[2 streams]**
No `-fsanitize` build option/target exists. For a heavily concurrent engine this is the most
important missing safety net — and it is why none of H1/M6/M8/M11/M12 would be caught by the
passing test suite (see T-series). At minimum add a TSan CI configuration.

### M16 (test) — Indicator numeric correctness is essentially unverified; the rigorous indicator tests run on DEAD code — CONFIRMED
`tests/core/IndicatorsTest.cpp` carefully validates `connectors/.../ta/Indicators.hpp` (Wilder
RSI, `ema_next`, `sma_lastN`) — but that header is **included by nothing in production**. The
live indicators are separate inline reimplementations in `MarketTA.cpp` (e.g. production RSI is
a stateless simple-average, `:146-158`, a *different algorithm* than the tested Wilder version).
`AtomicFunctionsTest.cpp:64-86` tests MACD/RSI/EMA/Bollinger/ATR/OBV **presence-only**
(`has_value()`), so a wrong formula would still pass. The well-tested code does not guard the
code that runs.
**Fix:** point numeric tests at the production `MarketTA` path (or unify the two implementations).

### M17 (test) — No genuine concurrency/stress test; AtomicStore locking is untested under real contention — CONFIRMED
`tests/integration/StressTest.cpp:17-31` uses 4 threads writing **disjoint** fields (fully
serialized, zero contention); `tests/core/AtomicStoreTest.cpp:109-126` uses one reader + one
writer and ends in `SUCCEED()` (asserts nothing). With the store's locks deleted these tests
still pass. No randomized-interleaving, multi-reader/multi-writer-same-key, or soak test exists
— the exact bug class the M-series concurrency findings live in.

### M18 (test) — Production connector lifecycle (double-start/stop) is untested; contract pinned only on a self-defining mock — CONFIRMED
`tests/engine/ConnectorLifecycleTest.cpp:121-128` asserts "double-start throws" only against an
in-test `MockConnector` written to throw. Real connectors don't: `SyntheticConnector::start()`
re-arms its timer with no guard; `MarketConnector::start()` is a no-op with **no lifecycle test
at all**.

---

## LOW

### L1 — `JsonValidator::requireMember` rejects valid `false` booleans — CONFIRMED (latent)
`include/gma/JsonValidator.hpp:14-23` — compares against a single `expectedType`, but RapidJSON
uses distinct `kTrueType`/`kFalseType`, so `requireMember(..., kTrueType)` throws on `false`.
Only the number case is exercised by tests, so latent.

### L2 — `expr::compileNode` is unbounded recursion — CONFIRMED (latent)
`src/core/Expr.cpp:13-118` — no depth cap. Bounded on the validated request path
(`MAX_TREE_DEPTH=32`), but `buildTree`/`buildNode` are public and skip validation, so an
unvalidated deeply-nested `Expr` can blow the stack. Add a depth cap.

### L3 — Expr `div`/`mod` can emit `inf` despite the "no NaN/inf" contract — CONFIRMED
`src/core/Expr.cpp:97-98` guards only exact-zero denominators; a tiny non-zero denominator with
a large numerator yields `inf`, contradicting `Expr.hpp:33`. The `FunctionMap` builtins use an
`EPSILON` guard (`BuiltinFunctions.cpp:146-149`) — inconsistent. Add an epsilon guard.

### L4 — `SyntheticConnector` timer lambda is a self-referential `shared_ptr` cycle (leak) — CONFIRMED
`connectors/synthetic/src/SyntheticConnector.cpp:64-65` — `*selfRef = [...selfRef...]{...}`
keeps the lambda + captured state alive for process lifetime even after `stop()`. Demo/test-only
connector, so low. Capture a `weak_ptr` and lock inside.

### L5 — RSI is simple-average (not Wilder); flat prices yield RSI=0 — CONFIRMED
`connectors/market/src/MarketTA.cpp:146-159` — diverges from the (unused) Wilder `rsi_update`
in `Indicators.hpp`; all-flat input gives `RSI=0` rather than a neutral value. Pick one
definition and align.

### L6 — EMA/MACD re-seed from the sliding window's oldest sample each tick — SUSPECTED
`connectors/market/src/MarketTA.cpp:119-126, 169-183` — the EMA seed is the oldest *retained*
price, so `ema_N`/`macd_*` depend on `taHistoryMax` and aren't a true carried EMA. Fine when
`maxHistory ≫ period`, wrong when comparable. Consider stateful incremental EMA (`ema_next`).

### L7 — `std::hash<RequestKey>` uses `idx<<32` (UB on 32-bit; no salt for ints) — CONFIRMED (latent)
`include/gma/server/RequestKey.hpp:113-119` — `idx<<32` is UB where `size_t` is 32-bit
(platform is x86_64, so latent); the int branch effectively has no variant salt.

### L8 — Aggregated L2 ladders are written but never read — CONFIRMED (latent)
`OrderBook::applyLevelSummary`/`applySnapshotAggregated` populate `bidsAgg_`/`asksAgg_`, but
`buildSnapshot`/`bestBid`/`forEachLevel` read only the per-order ladders. A source delivering
aggregated L2 updates dead state. Not hit by the two real feeds, so latent. Have the readers
fall back to aggregated ladders when per-order is empty.

### L9 — `ob.meta.is_stale`/`last_change_ms` never populated — CONFIRMED
`connectors/market/src/MarketConnector.cpp:125-148` never sets `snap.meta.stale`/`lastChangeMs`.
Even if the provider resolved meta keys (it doesn't — see H4), they'd always read 0.

### L10 — `quantizeTicks` maps out-of-range prices to 0 ticks silently — CONFIRMED
`connectors/market/src/book/OrderBookManager.cpp:21-27` — an absurd price quantizes to price 0
rather than being rejected; `validatePrice_`'s tight absolute tolerance also risks false
rejection of valid large prices.

### L11 — `ObMaterializer::imbalanceLevels` has no upper range clamp — SUSPECTED (latent)
`connectors/market/src/ob/ObMaterializer.cpp:164-183` — `ob.imbalance.levels.1-2000000000`
loops ~2e9 times. Only reachable if `eval()` is wired into the provider (currently it isn't —
H4), hence latent. Add a clamp.

### L12 (build) — `-Wall -Wextra` applied to only 2 of 6 targets — CONFIRMED
Warning flags cover `gma_engine` and `gma_connector_market`, **not** `gma_server` (main.cpp),
`gma_connector_synthetic`, `gma_tests`, or benchmarks. Warnings in the composition root and
test code are invisible. Also: no CI, so `GMA_WARNINGS_AS_ERRORS` (default OFF) never runs.

### L13 (build) — `[-Wreorder]` member-init order mismatch in `FeedServer` — CONFIRMED
`connectors/market/include/gma/server/FeedServer.hpp:52` vs ctor init list at
`src/server/FeedServer.cpp:341` — harmless today (no inter-member dependency) but should match
declaration order.

### L14 (build) — `tmpnam` in a test (ld warning, TOCTOU footgun) — CONFIRMED
`tests/core/ConfigNamespaceDispatchTest.cpp:18` — replace with `mkstemp`.

### L15 (build) — Orphaned per-module `tests/**/CMakeLists.txt` reference stale filenames — CONFIRMED
The root build uses `GLOB_RECURSE`; the 11 per-module test CMake files are unused and list
files that no longer exist (`ConfigTest.cpp`, `AtomicFunctionsTest.cpp`). Dead build files that
mislead. Also: `GLOB_RECURSE` means adding a source doesn't auto-reconfigure CMake.

### L16 (build) — Dockerfile pins `libboost-*1.83.0` at runtime — CONFIRMED
Runtime stage hard-pins boost soname; if `debian:stable-slim` drifts from 1.83 the image build
breaks. Build stage uses unpinned `-dev`.

### L17 (test) — `CorpusTest` can silently skip → green suite, 97 KB corpus unexercised — CONFIRMED
`tests/treebuilder/CorpusTest.cpp:66-69` `GTEST_SKIP()`s on relative-path miss; CMake never
copies the corpus next to the binary and `add_test` sets no `WORKING_DIRECTORY`
(`CMakeLists.txt:284`). Runs only when cwd is exactly `<repo>/build` or `<repo>`. Fix:
`file(COPY)` the corpus and turn the skip into a `FAIL`.

### L18 (test) — WS tests use fixed `sleep_for` as the only subscribe-readiness barrier — SUSPECTED (flake)
`tests/ws/WebSocketE2ETest.cpp:129`, `ClientSessionTest.cpp:331/390/442/467` rely on 50–80ms
sleeps before injecting `onTick`; under load the listener may not be registered yet and the
update never arrives. The correct `pool.drain()` barrier is used elsewhere
(`IntegrationTest.cpp:335`). Same class affects `IntervalTest.cpp:25-27,65-67` (wall-clock
count assertions).

### L19 (test) — Several tests pass for the wrong reason — CONFIRMED
e.g. `TreeBuilderTest.cpp:47-69` ("RejectsMissingId/Tree") all throw on a missing top-level
`streamKey` and never reach id/tree logic; `MarketFieldMapTest.cpp:147-169` proves nothing
about the custom map (its only assertion is satisfied by the default computer);
`WsBridgeTest.cpp:32-36` constructs `WsBridge(nullptr,nullptr)`. Misnamed/tautological coverage.

### L20 (test) — ITCH semantic-malformation and crossed-book are untested — CONFIRMED
Broken JSON is tested, but untyped `price` silently coerces to `0.0`
(`ItchAdapter.cpp:97-100`), and zero-share execute/cancel emit phantom events — untested.
`addImpl` never checks best-bid<best-ask and `checkInvariants` doesn't detect a crossed book;
no test crosses one.

---

## DOCUMENTATION MISMATCHES (all CONFIRMED against code)

These cause real client-integration failures (wrong keys/ports return nothing), so they are
treated as defects, not cosmetics.

- **D1 — `README.md` is wholesale stale.** Documents a different protocol
  (`clientId`/`seriesId`/`operations`), wrong binary name (`gma-server`), wrong port (9002),
  moved headers, and a fictional compile-time `struct Config`. Recommend rewrite or deletion.
- **D2 — `docs/atomic-keys.md:183-184` TA keys are wrong.** Documents `macd.line`/`bb.upper`;
  the engine emits `macd_line`/`macd_signal`/`macd_histogram` and only
  `bollinger_upper`/`bollinger_lower` (`MarketTA.cpp:187-218`). A client using the documented
  keys gets nothing. (`sma_N`/`ema_N`/`rsi_N`/`ob.*` parts match.)
- **D3 — `docs/ARCHITECTURE.md:278-291` contradicts ENC-50.** Documents subscribe as
  `{"key":1,"symbol":"AAPL"}` ("symbol kept deliberately"); the live protocol requires
  `streamKey` with no `symbol` fallback (`ClientSession.cpp:363-405`). The unused
  `include/gma/ws/JsonSchema.hpp:16,22` still mandates `required:["id","symbol"]` (dead code,
  also contradicts the wire format).
- **D4 — Default `wsPort` mismatch.** Docs say 4000; the compiled default is **8080**
  (`include/gma/util/Config.hpp:67`). 4000 applies only because `src/util/gma.conf` sets it, so
  `./gma_server` with no config listens on 8080.
- **D5 — `CLAUDE.md` SMA/EMA defaults contradict the file it cites.** Lists SMA{5,20}/EMA{12,26};
  `gma.conf:22-23` ships `taSMA=5,10,20,50` / `taEMA=9,21,50`.
- **D6 — Adapter docs use pre-ENC-31 paths.** `docs/feed-adapters.md`/`writing-adapters.md`
  cite engine-level `include/gma/feed/...` and `src/feed/...` paths that moved into the
  connector, reference the deleted `MarketDispatcher`, and show `main.cpp` adapter wiring (now
  ingress-factory-driven).
- **D7 — `SourceProfile.hpp` no longer exists** but is referenced by `CLAUDE.md:63` and
  `ARCHITECTURE.md:331`; the type is now `MarketFieldMap`.
- **D8 — `DELIVERABLES.md` reads as a live bug list but is a fixed historical audit.** Every
  headline bug it lists is fixed in current code, and it cites deleted paths
  (`src/core/AtomicFunctions.cpp`, `MarketDispatcher.cpp`). A reviewer treating it as current
  chases ghosts.
- **D9 — In-source comment at `TreeBuilder.cpp:312-318`** claims the `ob.*` reject surfaces as
  `where:"validate"`; it actually surfaces as `where:"build"` (the docs are right, the comment
  is wrong).
- **D10 — Undocumented config keys** parsed by `Config.cpp` but absent from `CLAUDE.md`:
  `logLevel`, `metricsEnabled`, `metricsIntervalSec`, `taATR/taMomentum/taVolAvg`,
  `allowNegativePrices`, the legacy `feed.*`/`ingress.*`, and `forumUrl/forumTenantId/forumAuthToken`.

---

## SUGGESTED CLOSE-OUT ORDER

1. **C1** (book corruption) and **H1** (shutdown hang) — correctness/availability, fix first.
2. **H2** (timer/thread leak) and **H3** (ODR/UB) — resource + UB, fix next.
3. **M15** — wire a **TSan build + CI job**; it is the missing safety net behind nearly every
   concurrency finding (M6/M8/M11/M12) and would prevent regressions.
4. **H4 + the doc key mismatches (D2/D3/D4)** — the advertised `ob.*`/TA keys and ports don't
   resolve; clients integrating against the docs get silent NaN/no-data. Fix the resolver and
   correct the docs together.
5. Remaining **M/L** items as scheduled hardening; fold the test gaps (M16–M18, L17–L20) into
   the same tickets as the code fixes they would have caught.

Each item above maps cleanly to a ≤5h Linear ticket; H4/D2/D3/D4 and the connector
order-book items (C1/M1/M2/M4/M6/M7) form two natural ticket clusters.

---

## LINEAR TICKETS

All findings worth resolving are tracked under the Linear project **"GMA_V3 Code-Review
Remediation 2026-06"** (ENC), clustered into 24 PR-sized (≤5h) tickets:

| Ticket  | Findings | Title |
|---------|----------|-------|
| ENC-785 | C1, M1   | Order book: ITCH executions & trade prints double-consume the book |
| ENC-786 | H4, L8, L9, L11 | Live ob.* provider resolves only 4 key families; ObMaterializer is dead code |
| ENC-787 | H1       | ThreadPool::drain() lost-wakeup can hang shutdown forever |
| ENC-788 | H3       | Duplicate gma::util::MetricRegistry — ODR violation / UB |
| ENC-789 | H2       | ClientSession teardown leaks keep-alive chain → timer-node threads leak |
| ENC-790 | H5, H6   | Docker image non-portable: -march=native + no .dockerignore |
| ENC-791 | M8, M11  | Dispatcher: unguarded _computers race + concurrent compute() |
| ENC-792 | M9, M10  | Dispatcher atomics drop field + unbounded per-tick builtins |
| ENC-793 | M12      | ClientSession drives one Beast stream on two strands |
| ENC-794 | M13      | Signal handler runs non-async-signal-safe shutdown |
| ENC-795 | M14      | forum empty/no-itch connectors boots with zero ingress (AC-6) |
| ENC-796 | M2       | ItchAdapter IsNumber() but calls GetUint64()/GetInt() — DoS |
| ENC-797 | M3, M4   | Market connector unbounded growth (maxSymbols bypass + orders_) |
| ENC-798 | M5       | FeedServer stop()/close() touch Asio objects off-strand |
| ENC-799 | M6       | OrderBookManager::mutateWithDelta_ not atomic under concurrency |
| ENC-800 | M7       | Sequence/gap handling (onSeq/onReset) never invoked by ingress |
| ENC-801 | M15      | Add sanitizer builds (TSan/ASan/UBSan) + CI gate |
| ENC-802 | M16      | Numeric indicator tests run against dead code, not MarketTA |
| ENC-803 | M17, M18 | Real-contention concurrency tests + connector lifecycle tests |
| ENC-804 | L1,L2,L3,L5,L6,L7,L10 | Engine/expr hardening bundle |
| ENC-805 | L4       | SyntheticConnector timer self-referential shared_ptr cycle |
| ENC-806 | L12-L16  | Build hygiene bundle |
| ENC-807 | L17-L20  | Test-suite fixes |
| ENC-808 | D1-D10   | Documentation accuracy pass |
