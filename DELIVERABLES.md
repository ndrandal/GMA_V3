# GMA_V3 Overhaul — 6 Deliverables

> Full audit of every source file. Issues ranked by severity. Each deliverable is a self-contained body of work that can be tackled independently.

---

## Deliverable 1: Core Engine & Computation Pipeline

**Scope:** `AtomicFunctions`, `AtomicStore`, `FunctionMap`, `MarketDispatcher`, `TreeBuilder`, `JsonValidator`

### Critical Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| `src/core/TreeBuilder.cpp` | 470-471 | **Duplicate `head->start()` call** — Listener is started twice, creating duplicate subscriptions and double-notifications |
| `TreeBuilder.hpp` | 49 | **`buildTree()` declared but never implemented** — linker error if called |
| `TreeBuilder.hpp` vs `.cpp` | 41-46 / 231 | **`buildOne()` declared public in header but `static` in implementation** — public API is a lie |
| `src/core/AtomicFunctions.cpp` | 98-100 | **MACD signal calculation is wrong** — copies `macdLine` into all history entries, then calls `ema()` which still references the original `hist` via captured reference. Signal line is computed on raw prices, not on MACD line values |
| `src/core/AtomicFunctions.cpp` | 160-164 | **Placeholder values hardcoded** — `isHalted=0`, `marketState="Open"`, `timeSinceOpen=60`, `timeUntilClose=300` are static lies, never computed |

### Thread-Safety Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `src/core/MarketDispatcher.cpp` | 49-100 | **Race in listener notification** — listeners collected under read lock, then dereferenced after lock is released. Unregistration between collection and call causes UAF |
| `src/core/MarketDispatcher.cpp` | 79-89 | **Dangling pointer to deque** — `histPtr` points into `_histories` map, used after lock is released. Map rehash or entry removal invalidates it |
| `src/core/MarketDispatcher.cpp` | 28-42 | **Potential deadlock** — if listener destructor calls `unregisterListener()`, re-enters already-held unique_lock |
| `FunctionMap.hpp` | 26 | Uses `std::mutex` instead of `std::shared_mutex` — every `get()` serializes all readers |

### Performance Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `src/core/MarketDispatcher.cpp` | 109 | **Copies entire FunctionMap on every tick** — `getAll()` allocates and copies all `std::function` objects per (symbol, field) pair per tick |
| `src/core/MarketDispatcher.cpp` | 116 | **Deque→vector copy on every tick** — full history copied to `std::vector<double>` for each atomic function call. Should use `Span<double>` |

### Code Quality

| File | Line(s) | Issue |
|------|---------|-------|
| `src/core/AtomicFunctions.cpp` | 77-82, 111, 127 | All TA periods hardcoded (SMA 5/20, EMA 12/26, RSI 14, BBands 20/2.0, ATR 14). Config exists but is never used here |
| `src/core/AtomicFunctions.cpp` | 11 | `computeMedian` takes `std::deque` by value instead of const ref |
| `src/core/TreeBuilder.cpp` | 278 | Unused variable `cap` with `(void)cap` suppression |
| `src/core/TreeBuilder.cpp` | 19, 21 | Duplicate `#include "gma/rt/ThreadPool.hpp"` |
| `src/core/TreeBuilder.cpp` | 293, 410 | Direct dependency on global `gma::gThreadPool` — untestable, violates DI |
| `src/core/MarketDispatcher.cpp` | 74, 120 | Bare `catch(...)` swallows all exceptions with zero logging |
| `src/core/JsonValidator.cpp` | — | Minimal validation: no size limits, no tree structure validation, no node type validation, no parameter range checks |
| `JsonValidator.hpp` | 12-22 | `requireMember` is a template on `T` but `T` is never used |

### Overhaul Plan

1. Fix duplicate `start()`, implement `buildTree()`, reconcile `buildOne()` visibility
2. Rewrite MACD signal: maintain rolling MACD-line history, compute 9-period EMA on that
3. Replace hardcoded TA periods with `Config` lookups
4. Fix MarketDispatcher thread safety: hold shared_lock through notification, or copy listener list under lock and notify outside
5. Replace `getAll()` copy with cached snapshot or `shared_mutex` read path
6. Use `Span<const double>` to eliminate deque→vector copies
7. Replace bare `catch(...)` with typed catches + logging
8. Expand `JsonValidator` to full schema validation (field types, ranges, tree structure)
9. Inject `ThreadPool*` via `Deps` struct instead of global

---

## Deliverable 2: Node Pipeline

**Scope:** `INode`, `Listener`, `Worker`, `Aggregate`, `Responder`, `AtomicAccessor`, `SymbolSplit`, `Interval`

### Won't-Compile Bugs

| File | Issue |
|------|-------|
| `Aggregate.hpp` | **Constructor takes `(size_t arity, shared_ptr<INode>)` but tests pass `(vector<shared_ptr<INode>>, shared_ptr<INode>)`** — all Aggregate tests fail to compile |
| `Worker.hpp` | **Constructor takes `(Fn, shared_ptr<INode>)` but tests pass `(Fn, vector<shared_ptr<INode>>)`** — all Worker tests fail to compile |
| `Worker.hpp` | **Type alias is `Fn` but tests reference `Worker::Function`** — name mismatch |
| `ThreadPool.hpp` | **No `shutdown()` method exists** — tests call `pool.shutdown()` which doesn't exist (only `drain()` exists). Blocks Interval and Listener tests |

### Logic Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| `src/nodes/Worker.cpp` | 8-21 | **Worker applies function immediately on every single value and clears accumulator** — `vec.size()` is always 1 when function is called. Batching does not work. Comments describe batching, code does immediate pass-through |
| `src/nodes/Listener.cpp` | — | **Tests never call `start()` after construction** — header explicitly warns this is required. Without it, Listener never registers with MarketDispatcher, so ticks never arrive |

### Thread-Safety Bugs

| File | Issue |
|------|-------|
| `src/nodes/Aggregate.cpp` | `buf_` is `unordered_map` with **no mutex** — concurrent `onValue()` calls cause data races and undefined behavior |
| `src/nodes/Worker.cpp` | `acc_` is `unordered_map` with **no mutex** — same problem as Aggregate |
| `src/nodes/Interval.cpp` | `pool_->post()` called with raw `pool_` pointer. If pool is destroyed while Interval lives, **use-after-free** |
| `src/nodes/Listener.hpp` | Both `pool_` and `dispatcher_` are raw pointers with **no lifetime guarantees** |

### Design Issues

| File | Issue |
|------|-------|
| `src/nodes/Interval.cpp` | **`std::this_thread::sleep_for()` inside thread pool task** — blocks a pool thread for the entire interval period. 4 Intervals = 4 blocked threads = pool starvation |
| `src/nodes/Interval.cpp` | **No graceful shutdown wait** — `shutdown()` sets flag but doesn't wait for in-flight work. Caller doesn't know when Interval truly stopped |
| `Responder.hpp` | `_send` callback can be null — calling empty `std::function` throws `std::bad_function_call` with no null check |
| `AtomicAccessor.hpp` | `store_` is raw pointer, null-checked with silent return — no error propagation |
| `Responder.hpp` | Uses `_leading_underscore` naming while all other nodes use `trailing_underscore_` |
| All nodes | Downstream held as `weak_ptr` — lock failure causes silent data loss with no logging |

### Overhaul Plan

1. **Fix constructors**: Reconcile Aggregate and Worker signatures with test expectations (vector-based children)
2. **Add `ThreadPool::shutdown()`**: Implement proper shutdown that sets stopping flag, drains queue, and joins threads
3. **Fix Worker batching**: Either wire Aggregate→Worker for N-ary, or implement proper batch accumulation with configurable flush trigger
4. **Add mutexes to Aggregate and Worker**: Follow SymbolSplit's pattern (it already correctly uses `std::mutex`)
5. **Replace raw pointers with `shared_ptr`**: `pool_`, `dispatcher_`, `store_` should all be shared or weak pointers with lifetime guarantees
6. **Replace Interval's `sleep_for` with timer**: Use `boost::asio::steady_timer` to avoid blocking pool threads
7. **Add null checks and error logging**: Responder callback, downstream lock failures, store access failures
8. **Fix Listener tests**: Call `start()` after construction in all test cases
9. **Normalize naming**: All private members use `trailing_underscore_` convention

---

## Deliverable 3: Server & WebSocket Layer

**Scope:** `WebSocketServer`, `ClientSession`, `FeedServer`, `WsBridge`, `ClientConnection`, `WSResponder`, `JsonSchema`

### Critical Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| `src/ws/WsBridge.cpp` | 13 | **`onMessage()` is a TODO stub** — entire message handling bridge is unimplemented. All WebSocket messages silently dropped |
| `src/ob/ObMaterializer.cpp` | 240 | **`meta()` function declared but never defined** — linker error when Metric::Meta is evaluated |
| `src/ob/ObMaterializer.cpp` | 242-244 | **VWAP and Cumulative metrics return NaN** — `eval()` has no case for `Metric::Cum` or `Metric::VWAP`, falls through to default returning `quiet_NaN()` |

### Duplicate Abstractions

| Files | Issue |
|-------|-------|
| `include/gma/ws/ClientConnection.hpp` vs `include/gma/ClientConnection.hpp` | **Two different ClientConnection classes in the same namespace** — different constructors, different callback sets (OnOpen/OnClose/OnError vs OnMessage only), different methods. Code could link against wrong version depending on include order |

### Thread-Safety & Protocol Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `src/server/ClientSession.cpp` | 226-239 | **Unbounded outbox queue** — `outbox_.push_back(s)` has no size limit. Slow/dead client causes unbounded memory growth → OOM |
| `src/server/ClientSession.cpp` | 167-168 | **Buffer not consumed on exception** — if `buffers_to_string()` throws, `consume()` is never called, buffer leaks |
| `src/server/ClientSession.cpp` | 330-387 | **`handleSubscribe()` is 180+ lines mixing JSON parsing, tree building, node creation, and callback management** — tight coupling, untestable |
| `src/server/FeedServer.cpp` | 75-106 | **`handleLine()` has no try-catch** — exception from `onTick()` or missing JSON key crashes the FeedSession |
| `src/ws/ClientConnection.cpp` | 229 | **Error fallback to `std::cerr`** instead of structured logger |

### Missing Features

| Feature | Impact |
|---------|--------|
| No heartbeat / ping-pong | Dead connections hang indefinitely. No detection of half-open TCP |
| No rate limiting | Clients can spam subscribe requests, exhaust server resources |
| No backpressure | Fast producer + slow consumer = OOM |
| No connection state machine | States tracked implicitly via `open_` and `writing_` booleans. No validation of legal transitions |
| `JsonSchema.hpp` schemas defined but never used | Client requests bypass all schema validation |

### Overhaul Plan

1. **Implement `WsBridge::onMessage()`** or remove the class entirely
2. **Implement `ObMaterializer::meta()`, VWAP, and Cum eval branches**
3. **Consolidate ClientConnection**: Delete the legacy version, keep the full implementation
4. **Add backpressure to outbox**: Max queue depth (e.g., 1024), drop oldest or close connection when full
5. **Add WebSocket ping/pong**: Server-side heartbeat on a 30s timer, close connections that don't respond
6. **Add rate limiting**: Per-connection message rate and subscription count limits
7. **Refactor `handleSubscribe()`**: Extract into `RequestHandler` class with parse → validate → build → respond stages
8. **Wire `JsonSchema` into validation pipeline**: Validate all incoming requests against defined schemas before processing
9. **Add try-catch to `FeedServer::handleLine()`**: Log and continue on individual line failures
10. **Add connection state machine**: Explicit `enum class State { Handshaking, Open, Closing, Closed }` with transition validation

---

## Deliverable 4: Order Book System

**Scope:** `OrderBook`, `OrderBookManager`, `DepthTypes`, `ObEngine`, `ObKey`, `ObMaterializer`, `ObProvider`, `ObSnapshot`

### Thread-Safety Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| `src/book/OrderBookManager.cpp` | 84-87 | **`gate_()` reads `feed_` map without lock** — `feed_` is guarded by `booksMx_` everywhere else, but `gate_()` is `const` and skips the lock. Race with `onReset()` writing `stale=true` |
| `src/book/OrderBookManager.cpp` | 55-74 | **`onSeq()` accesses `feed_` map without lock** — `auto& st = feed_[symbol]` performs concurrent insert+read without synchronization. Map rehash during concurrent access = UB |

### Correctness Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `src/book/OrderBook.cpp` | 285-299 | **`addImpl()` accepts orders with size=0 and price=0** — no validation. Zero-size orders corrupt level aggregates |
| `src/book/OrderBook.cpp` | 302-352 | **`updateImpl()` iterator invalidation risk** — `getOrCreateLevel()` may cause map operations while `it->second` references into `byId_`. Safe for `std::unordered_map` in practice but fragile |
| `src/ob/ObMaterializer.cpp` | 209-244 | **`eval()` missing Cum and VWAP cases** — returns NaN for these metric types |
| `src/ob/ObMaterializer.cpp` | — | **`cumLevels()`, `vwapLevels()`, `vwapPriceBand()`, `meta()` declared but not implemented** — linker errors |

### Missing Tests

The **entire** order book system has **zero tests**:
- `OrderBook.cpp` — untested
- `OrderBookManager.cpp` — untested
- `ObEngine.cpp` — untested
- `ObKey.cpp` — untested
- `ObMaterializer.cpp` — untested
- `ObProvider.cpp` — untested

### Overhaul Plan

1. **Fix `gate_()` and `onSeq()`**: Acquire `booksMx_` (shared_lock for reads, unique_lock for writes)
2. **Add order validation in `addImpl()`**: Reject size=0, validate price range, validate side
3. **Implement missing ObMaterializer functions**: `cumLevels()`, `vwapLevels()`, `vwapPriceBand()`, `meta()`
4. **Add eval() cases for Cum and VWAP metrics**
5. **Write comprehensive test suite**:
   - OrderBook: add/update/delete, cancel-replace, level aggregation, price priority
   - OrderBookManager: sequence gaps, stale detection, reset recovery
   - ObKey: key parsing for all metric types
   - ObMaterializer: eval for every Metric enum value
   - ObEngine: snapshot generation, provider registration
6. **Add `OrderKeyHash` quality tests**: Verify collision rate and distribution

---

## Deliverable 5: Runtime Infrastructure & Utilities

**Scope:** `ThreadPool`, `SPSCQueue`, `ShutdownCoordinator`, `Config`, `Logger`, `Metrics`, `TAComputer`, `Indicators`

### Critical Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| `src/util/Logger.cpp` | 114-120 | **`Logger::Scoped` destructor is a no-op** — constructor adds keys to thread-local map, destructor does nothing. Context keys leak indefinitely, never cleaned up. Memory leak on every Scoped construction |
| `src/rt/ThreadPool.cpp` | 32-35 | **`drain()` returns when queue is empty, not when work is done** — workers may still be executing dequeued tasks. Caller thinks pool is idle when it isn't |
| `SPSCQueue.hpp` | 48-49 | **Inconsistent memory ordering in `empty()` and `full()`** — use default `seq_cst` or `acquire` inconsistently with `try_push()`/`try_pop()` which use `relaxed`/`acquire`. Data races possible |
| `src/util/Config.cpp` | 88 | **`loadFromFile()` always returns true** — `ok` initialized true, never set false. Parse errors are invisible to caller |
| `src/util/Config.cpp` | 50 | **Dead code**: 8KB buffer allocated and never used |

### Thread-Safety Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `SPSCQueue.hpp` | 18-33 | **Missing acquire fence before `buf_[]` access** in both `try_push()` and `try_pop()`. Producer might read stale data from consumer's slot, consumer might read incomplete write from producer |
| `include/gma/util/Metrics.hpp` | 40-44 | **Race on `thr_` assignment** — CAS sets `running_=true`, then assigns `thr_` outside the CAS. If `stopReporter()` called between CAS and assignment, `thr_` is uninitialized |
| `include/gma/util/Config.hpp` | 18-24 | Config fields are non-const, non-atomic. Concurrent read during `loadFromFile()` is a data race |

### Design Issues

| File | Line(s) | Issue |
|------|---------|-------|
| `include/gma/util/Metrics.hpp` | 79-89 | **Reporter thread does nothing** — `reporterLoop()` sleeps in a loop but never actually reports. Comment says "intentionally do not print" |
| `src/rt/ThreadPool.cpp` | 26 | **`post()` silently drops tasks when stopping** — no return value, no logging, caller never knows |
| `src/rt/ThreadPool.cpp` | 46 | **Worker `catch(...)` swallows all exceptions** — no logging of what failed |
| `include/gma/runtime/ShutdownCoordinator.hpp` | 30-31 | **`std::sort` is unstable** — steps with equal `order` may execute in arbitrary order between runs |
| `include/gma/runtime/ShutdownCoordinator.hpp` | 38 | **Shutdown step failures swallowed silently** — `catch(...)` with no logging |
| `src/util/Logger.cpp` | 77-109 | **JSON escaping incomplete** — doesn't escape newlines, backslashes, or control characters. Produces invalid JSON output |
| `src/util/Logger.cpp` | 50 | **`setFile()` leaks file handle** if called multiple times (first close is correct, but destructor never closes) |
| `include/gma/ta/Indicators.hpp` | 100-107 | **RSI `init` flag never set to true** — caller must manage initialization state externally but gets no signal. RSI stays in init mode returning NaN forever unless caller knows the period |
| `include/gma/ta/TAComputer.hpp` | — | **TAComputer doesn't use Indicators.hpp** — SymState is a placeholder struct. TA functions exist but aren't wired into the state machine |

### Overhaul Plan

1. **Fix `Logger::Scoped`**: Track added keys in destructor, remove them on scope exit
2. **Fix `drain()`**: Add completion counter — increment on dequeue, decrement on task finish. `drain()` waits for counter==0 AND queue empty
3. **Add `ThreadPool::shutdown()`**: Set stopping, notify all, join threads. Return `bool` from `post()` to indicate acceptance/rejection
4. **Fix SPSCQueue memory ordering**: Uniform acquire/release semantics on all atomic operations. Add acquire fence before `buf_[]` reads
5. **Fix Config**: Return `false` on parse error. Remove dead buffer. Use `std::stoi`/`std::stod` with try-catch instead of `atoi`/`atof`
6. **Fix Logger JSON escaping**: Escape `\n`, `\r`, `\t`, `\\`, `\"` and control characters
7. **Add Logger destructor**: Close file handle if not stdout
8. **Fix Metrics reporter**: Either implement actual reporting (write to log/file) or remove the thread entirely
9. **Fix Metrics `startReporter()` race**: Protect `thr_` assignment with mutex
10. **Wire TAComputer to Indicators**: Integrate SMA/EMA/RSI state tracking into `SymState`, call indicator functions from `onTick()`
11. **Fix RSI**: Return init completion signal or manage period count internally
12. **Use `std::stable_sort`** in ShutdownCoordinator for deterministic equal-order execution
13. **Add logging to all `catch(...)` blocks** in ThreadPool and ShutdownCoordinator

---

## Deliverable 6: Build System & Test Suite

**Scope:** `CMakeLists.txt` (root + all subdirs), all test files, `tools/` scripts, `src/main.cpp`

### Won't-Build Bugs

| File | Line(s) | Issue |
|------|---------|-------|
| Root `CMakeLists.txt` | — | **Missing `find_package(GTest REQUIRED)`** — test target links `GTest::gtest_main` but GTest is never found. Linker error |
| `tests/core/ConfigTest.cpp` | 1 | **Wrong include path** — `#include "gma/Config.hpp"` should be `"gma/util/Config.hpp"` |
| `tests/core/LoggerTest.cpp` | 1 | **Wrong include path** — `#include "gma/Logger.hpp"` should be `"gma/util/Logger.hpp"` |
| `tests/treebuilder/TreeBuilderTest.cpp` | 4 | **Typo in include** — `"gma/rtThreadPool.hpp"` should be `"gma/rt/ThreadPool.hpp"` |
| `tests/core/ConfigTest.cpp` | 8-40 | **References non-existent statics** — `Config::ListenerQueueMax`, `Config::HistoryMaxSize`, `Config::ThreadPoolSize` don't exist in Config class |
| `tests/core/LoggerTest.cpp` | 29-109 | **References non-existent static methods** — `Logger::info()`, `Logger::warn()`, `Logger::error()` don't exist. Logger uses instance method `log(LogLevel, ...)` |
| `tests/CMakeLists.txt` | 12 | **Windows-only** — `set(GTEST_CTEST_RUNNER cmd ...)` breaks Linux/macOS |

### Broken Tests

| File | Issue |
|------|-------|
| All Aggregate tests | Constructor signature mismatch (see Deliverable 2) |
| All Worker tests | Constructor signature mismatch + type alias mismatch (see Deliverable 2) |
| All Interval tests | `pool.shutdown()` doesn't exist (see Deliverable 2) |
| All Listener tests | `pool.shutdown()` doesn't exist + `start()` never called |
| `tests/integration/IntegrationTest.cpp` | **Empty file** — 0 lines |
| `tests/integration/StressTest.cpp` | **Stub** — imports only, no test cases |

### Build System Design Issues

| Issue | Detail |
|-------|--------|
| **Two conflicting test strategies** | Root CMakeLists uses `GLOB_RECURSE` for monolithic `gma_tests`. `tests/CMakeLists.txt` uses per-module `add_subdirectory()`. They can't coexist |
| **Tests OFF by default** | `option(GMA_BUILD_TESTS ... OFF)` with no documentation on how to enable |
| **C++ standard mismatch** | CMake sets C++17 (`CMAKE_CXX_STANDARD 17`) but README claims C++20 |

### main.cpp Critical Issues

| Line(s) | Issue |
|---------|-------|
| 147-151 | **Use-after-free at shutdown** — shutdown lambdas capture `ws`, `feed`, `ioc` by reference. These are stack-local in `main()`. If shutdown runs after `main()` returns (e.g., from atexit or signal handler after stack unwind), all references are dangling |
| 137-142 | **`ws.run()` and `feed.run()` called sequentially** — if `run()` is blocking, feed server never starts. If non-blocking (just schedules ASIO work), this is fine but unclear |
| 118 | Thread pool size hardcoded to 4 — should come from config |
| 141 | Feed port hardcoded to 9001 — not configurable from CLI |
| 91-104 | Port validation: `stoul` → cast to `unsigned short` can silently overflow |

### Test Coverage Gaps

| Component | Files | Status |
|-----------|-------|--------|
| Order Book | `OrderBook.cpp`, `OrderBookManager.cpp` | **0 tests** |
| OB Engine | `ObEngine.cpp`, `ObKey.cpp`, `ObMaterializer.cpp`, `ObProvider.cpp` | **0 tests** |
| Server | `ClientSession.cpp`, `FeedServer.cpp`, `WebSocketServer.cpp` | **0 tests** |
| WebSocket | `WsBridge.cpp`, `WSResponder.cpp` | **0 tests** |
| TA | `TAComputer.cpp` | **0 tests** |
| Metrics | `Metrics.cpp` | **0 tests** |
| Integration | `IntegrationTest.cpp`, `StressTest.cpp` | **Empty stubs** |

### Tools Issues

| File | Issue |
|------|-------|
| All Python scripts | No `mkdir -p artifacts/` before writing — silent failure if directory missing |
| All Python scripts | `except: pass` swallows all errors silently |
| `node_map.py` | Identical copy of `atomic_map.py` — duplicated file |
| `compile.sh` | References undefined `-DGMA_STRICT=ON` option |
| `repo_inventory.sh` | Writes to `artifacts/` without creating it |

### Overhaul Plan

1. **Fix CMake**:
   - Add `find_package(GTest REQUIRED)` (or `FetchContent` for auto-download)
   - Pick one test strategy (recommend monolithic `gma_tests` from root)
   - Remove Windows-only `GTEST_CTEST_RUNNER` or make it conditional
   - Set `CMAKE_CXX_STANDARD 20` to match actual usage
   - Document test build: `cmake .. -DGMA_BUILD_TESTS=ON`
2. **Fix all test include paths**: `Config.hpp` → `util/Config.hpp`, `Logger.hpp` → `util/Logger.hpp`, `rtThreadPool.hpp` → `rt/ThreadPool.hpp`
3. **Fix test API mismatches**: Rewrite ConfigTest and LoggerTest to match actual class interfaces
4. **Fix main.cpp**:
   - Move `ws`, `feed`, `ioc` to longer-lived scope or use `shared_ptr`
   - Make feed port configurable
   - Read thread pool size from config
   - Add port range validation
5. **Write missing tests** (priority order):
   - OrderBook + OrderBookManager (data integrity)
   - ObMaterializer + ObKey (computation correctness)
   - Integration tests (end-to-end Listener→Worker→Responder pipeline)
   - Server/WebSocket (mock-based session tests)
   - Stress tests (concurrent subscriptions, high tick rate)
6. **Fix tools**:
   - Add `os.makedirs("artifacts", exist_ok=True)` to all Python scripts
   - Replace bare `except: pass` with `except Exception as e: print(f"Error: {e}")`
   - Delete duplicate `node_map.py` or differentiate it
   - Fix `compile.sh` to use actual CMake options

---

## Priority Matrix

| Deliverable | Blocking? | Effort | Risk if Ignored |
|-------------|-----------|--------|-----------------|
| **D6: Build & Tests** | YES — nothing compiles | Medium | Can't validate anything |
| **D2: Node Pipeline** | YES — core pipeline broken | High | No data flows end-to-end |
| **D1: Core Engine** | YES — wrong computations | High | Silent wrong results to clients |
| **D5: Runtime/Utils** | Partial — data races | Medium | Intermittent crashes, memory leaks |
| **D3: Server/WS** | Partial — stubs exist | High | Messages silently dropped, no heartbeat |
| **D4: Order Book** | Partial — race conditions | Medium | Stale/corrupt book data |

**Recommended execution order: D6 → D2 → D1 → D5 → D3 → D4**

Fix the build first so you can run tests. Fix the node pipeline so data can flow. Fix computations so results are correct. Then harden runtime, server, and order book.
