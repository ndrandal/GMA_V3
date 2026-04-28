# GMA_V3 Architecture

This document is the authoritative reference for how GMA_V3 is put together. `CLAUDE.md` is the scannable orientation; this is the deeper picture.

## 1. Design intent

GMA_V3 is a **streaming-computation server**: JSON request trees come in over WebSocket, events come in from data feeds, and the node pipeline runs user-specified math on the stream and pushes results back out. The codebase is split into two layers:

- **Engine** (`libgma_engine`): everything that has no idea what it's processing — node pipeline, event routing, JSON tree builder, WebSocket transport, thread pool, atomic store.
- **Connectors** (`libgma_connector_market`, `libgma_connector_synthetic`, …): everything domain-specific — feed protocols, per-event computations (e.g. TA), order books, custom atomic provider namespaces, specialized node types.

**The core invariant:** adding a new connector touches only the connector's own directory plus one line at the composition root. No engine edits. If a change to the engine is ever required to support a new connector, something went wrong in the extension-point design.

## 2. Library topology

```
┌──────────────────────────────────────────────────────────────┐
│ gma_server  (composition root; main.cpp, ~160 lines)         │
└──────────────────────┬───────────────────────────────────────┘
                       │ links
                       ▼
┌──────────────────────────────────────────────────────────────┐
│ gma  (INTERFACE target — transitive convenience alias)       │
└───────────┬──────────────────────┬───────────────────────────┘
            │                      │
            ▼                      ▼
┌───────────────────────┐ ┌────────────────────────────────────┐
│ libgma_connector_     │ │                                    │
│   market              │ │                                    │
│   (OB, TA, ITCH,      │ │                                    │
│    FeedServer,        │ │                                    │
│    WsFeedClient)      │ │                                    │
└──────┬────────────────┘ │                                    │
       │ depends on       │                                    │
       ▼                  ▼                                    │
┌──────────────────────────────────────────────────────────────┐
│ libgma_engine                                                │
│   Dispatcher, TreeBuilder, Node pipeline, WebSocketServer,   │
│   AtomicStore, FunctionMap, AtomicProviderRegistry,          │
│   ThreadPool, ShutdownCoordinator, registries under engine/  │
└──────────────────────────────────────────────────────────────┘
```

`libgma_connector_synthetic` sits next to the market connector (also depending on `gma_engine`) but is only linked into `gma_tests` — it exists as a demo + regression check, not a production feature.

Engine isolation is enforced via CMake include scoping: `gma_engine`'s PUBLIC include path is `include/` only, so any accidental `#include "gma/book/..."` or `#include "gma/market/..."` from inside engine code fails at compile time.

## 3. Event lifecycle (the hot path)

An `Event` is the canonical ingress unit:

```cpp
struct Event {
  std::string                          symbol;   // opaque stream key
  std::shared_ptr<rapidjson::Document> payload;  // source JSON
  std::string                          type { "tick" };
};
```

Where events come from, end-to-end, using the market connector as the example:

```
  TCP socket (port 9001)                External wss:// feed
           │                                    │
           ▼                                    ▼
   FeedServer.handleLine                   WsFeedClient
   (market JSON schema)                    + ItchAdapter.translate()
           │                                    │
           ▼                                    ▼
                      Event{ symbol, payload, type="tick" }
                                  │
                                  ▼
                ┌─── Dispatcher.onTick(event) ───┐
                │                                │
                │  1. For each IEventComputer c: │
                │       if c.eventType() ==      │
                │          event.type, run       │
                │          c.compute(event, ctx) │
                │                                │
                │     MarketTickComputer:        │
                │       - extract price/vol/bid/ │
                │         ask from payload       │
                │       - update per-symbol      │
                │         TickEntry history      │
                │       - run computeAll-        │
                │         AtomicValues → store   │
                │       - notifyListeners(       │
                │         symbol, "sma_5", v)    │
                │                                │
                │  2. For each raw payload field │
                │     matching a subscribed      │
                │     (symbol, field) listener:  │
                │       - append to per-field    │
                │         history                │
                │       - run FunctionMap        │
                │         builtins → store       │
                │       - threadPool.post(       │
                │         listener.onValue(sv))  │
                └────────────────────────────────┘
                                  │
                                  ▼
                        Listener.onValue()
                                  │
                                  ▼
                     downstream pipeline nodes
                                  │
                                  ▼
                           Responder → sendFn
                                  │
                                  ▼
                        ClientSession.sendText
                                  │
                                  ▼
                            async_write → WS
```

Key semantics:

- **Type routing.** `Dispatcher` invokes only computers whose `eventType()` matches the event's `type`. This is how market + synthetic computers coexist on the same dispatcher.
- **TA atomics are not directly subscribable.** `sma_5`, `rsi_14`, etc. are written to `AtomicStore` but delivered to listeners only when a client adds an `AtomicAccessor` node to its pipeline. Subscribing directly on field `sma_5` does nothing by itself.
- **FunctionMap builtins are subscribable.** `mean`, `sum`, `stddev`, etc. are computed per-field per-tick inside `Dispatcher::computeAndStoreAtomics` and fan out to matching listeners.
- **Per-field raw path.** If a listener subscribed on `(AAPL, lastPrice)` and the payload has `lastPrice`, the dispatcher reads it and delivers directly — no TA involvement.

## 4. Engine registries (extension points)

All live under `include/gma/engine/`. Most are header-only static singletons with a mutex-guarded map. Connectors publish here during `registerWith()`.

| Registry | Purpose | Who consumes |
|---|---|---|
| `EventTypeRegistry` | name → `EventSchema` (known fields, dispatchable flag) | Future validator integration |
| `EventComputerRegistry` | event type → list of `IEventComputer` **factories** (not instances — each dispatcher gets a fresh set, for state isolation) | Used as a scaffold today; `Dispatcher` currently consults the `DefaultComputerFactory` hook instead — see §5 |
| `NodeTypeRegistry` | type name → `NodeBuilderFn(spec, defaultSymbol, Deps, downstream) → shared_ptr<INode>` | `TreeBuilder::buildOne` + `JsonValidator::validateNode` |
| `AtomicProviderRegistry` | namespace (e.g. `"ob"`) → `double(symbol, fullKey)` | `AtomicAccessor` when a store lookup misses |
| `FunctionMap` | fn name (e.g. `"mean"`) → `double(vector<double>)` | `TreeBuilder`'s worker builder |
| `IngressRegistry` | kind (e.g. `"market-tcp-feed"`) → `IngressFactory(io, dispatcher, cfg)` | Reserved for future config-driven ingress creation |
| `ConfigNamespaceRegistry` | prefix (e.g. `"source"`) → reader function | Reserved for namespaced connector config |

**What's actually used today by the hot path:** `NodeTypeRegistry`, `AtomicProviderRegistry`, `FunctionMap`, and the `Dispatcher::DefaultComputerFactory` hook. The other registries are scaffolding — they work, have unit tests, and are ready to replace direct wiring when we pull more config/construction through them.

## 5. Connector contract

### Interface

```cpp
namespace gma::engine {

struct EngineRegistries {
  const util::Config*      cfg;
  rt::ThreadPool*          pool;
  AtomicStore*             store;
  Dispatcher*              dispatcher;
  rt::ShutdownCoordinator* shutdown;
  boost::asio::io_context* io;
};

class IConnector {
public:
  virtual ~IConnector() = default;
  virtual std::string_view name() const = 0;
  virtual void registerWith(EngineRegistries&) = 0;
};

}
```

`EngineRegistries` bundles the live, per-boot pieces. Static registries (`NodeTypeRegistry`, `FunctionMap`, …) are not in the struct — connectors access them through their singletons.

### Boot sequence (see `src/main.cpp`)

1. **Engine builtins** — `registerBuiltinFunctions()` populates `FunctionMap`; `registerBuiltinNodeTypes()` populates `NodeTypeRegistry`.
2. **Connector-side pre-dispatcher hooks** — e.g. `MarketConnector::installDefaults()` sets `Dispatcher::setDefaultComputerFactory(...)`. This **must** happen before any `Dispatcher` is constructed — the factory is consulted inside `Dispatcher`'s constructor so each new dispatcher gets its own fresh set of stateful computers.
3. **Engine components** — `ThreadPool`, `AtomicStore`, `Dispatcher`, `io_context`, `WebSocketServer`, `ExecutionContext`. Register shutdown steps.
4. **Connector registration** — construct connector objects and call `registerWith(regs)`. This is the "one line per connector" moment:

   ```cpp
   gma::engine::EngineRegistries regs{
     &cfg, gma::gThreadPool.get(), store.get(),
     dispatcher.get(), &shutdown, &ioc
   };
   gma::market::MarketConnector marketConnector;
   marketConnector.registerWith(regs);
   // (future) gma::crypto::CoinbaseConnector{}.registerWith(regs);
   ```

5. **Run** — `ioc.run()` until signal; `ShutdownCoordinator::stop()` drains registered steps in order.

### What `MarketConnector::registerWith` actually does (~100 lines)

- `installDefaults()` (idempotent re-install) — the TA tick-computer factory
- Constructs `OrderBookManager` + `FunctionalSnapshotSource` + `ob::Provider`
- Registers `"ob"` namespace with `AtomicProviderRegistry` so `AtomicAccessor` can fetch OB-derived keys (`ob.bestBid`, `ob.spread`, …)
- Constructs and starts `FeedServer` on `cfg.feedPort` (TCP market-JSON ingest)
- For each entry in `cfg.feeds`, constructs an `ItchAdapter` and `WsFeedClient`, starts it
- Registers shutdown steps: `"ob-provider-clear"`, `"feed-stop"`, `"feed-ws-stop"` (one per client)

## 6. Adding a new connector (cookbook)

Concrete recipe to add a connector named `cool`:

1. **Directory layout**

   ```
   connectors/cool/
     include/gma/cool/CoolConnector.hpp
     src/CoolConnector.cpp
   ```

2. **CMake** — append to `CMakeLists.txt`:

   ```cmake
   file(GLOB_RECURSE GMA_COOL_SOURCES "${CMAKE_SOURCE_DIR}/connectors/cool/src/*.cpp")
   add_library(gma_connector_cool STATIC ${GMA_COOL_SOURCES})
   target_include_directories(gma_connector_cool PUBLIC
     "${CMAKE_SOURCE_DIR}/connectors/cool/include")
   target_link_libraries(gma_connector_cool PUBLIC gma_engine)
   ```

   Link into `gma` (if you want the prod binary to use it) and/or into `gma_tests`.

3. **Implement `CoolConnector : public engine::IConnector`.** The `registerWith(EngineRegistries&)` method decides what you publish:

   - **Custom atomic namespace:** `AtomicProviderRegistry::registerNamespace("cool", ...)`.
   - **Custom node type:** `engine::NodeTypeRegistry::registerNodeType("CoolNode", builder)`. JSON trees can now specify `"type":"CoolNode"`.
   - **Per-event computation:** write a class that implements `engine::IEventComputer`; then either `reg.dispatcher->addComputer(std::make_unique<YourComputer>())` for a per-instance add, or install via a `Dispatcher::setDefaultComputerFactory` hook if you want every dispatcher in the program to get one.
   - **Ingress source:** construct your listener/timer/client against `reg.io` and register a shutdown step on `reg.shutdown`.

4. **Add one line to `main.cpp`** (the composition root):

   ```cpp
   gma::cool::CoolConnector coolConnector;
   coolConnector.registerWith(regs);
   ```

That's the entire integration. You shouldn't need to touch any file under `include/` or `src/`.

**Reference implementations:**
- `connectors/market/` — full production connector with OB, TA, two feed paths, custom provider namespace.
- `connectors/synthetic/` — ~120 LOC proof-of-concept. Emits a sine wave on a timer. Looked at alongside `tests/connectors/SyntheticConnectorTest.cpp` it's a complete worked example.

## 7. Node pipeline

`INode` has two methods:

```cpp
class INode {
  virtual void onValue(const StreamValue& sv) = 0;
  virtual void shutdown() noexcept = 0;
};
```

Built-in node types (all registered by `registerBuiltinNodeTypes()`):

| Type | Role |
|---|---|
| `Listener` | Head of a chain. Subscribes on `(symbol, field)`; `Dispatcher` calls its `onValue` when the field fires. Uses `weak_ptr` downstream to allow the session to drop the chain. |
| `Worker` | Runs a named function (from `FunctionMap`) across its accumulated inputs; emits downstream. |
| `Aggregate` | Fan-in of N input heads into one downstream; emits when all N inputs have reported for a tick cycle. |
| `Interval` | Timer wrapper — ticks its downstream every N ms (built on the engine thread pool). |
| `AtomicAccessor` | Pull-style — reads `(symbol, field)` from `AtomicStore` or the `AtomicProviderRegistry` and emits downstream. |
| `Responder` | Tail — writes the value back out to the WS client via a captured send function. |
| `GroupSplit` | Fans a single chain out to per-key child chains (constructed lazily on first key). **JSON wire name retained as `"SymbolSplit"`** for backward compatibility. |
| `Chain` | Sequential composition of stages (pipeline spec). |

### Ownership model

`ClientSession → Listener → ... → Responder → ClientSession` is a cycle. Break rule:

- **Listener** holds its downstream as `weak_ptr<INode>`. Cycle broken here.
- All other nodes hold downstream as `shared_ptr<INode>` (chain keeps itself alive).
- `TreeBuilder::BuiltChain.keepAlive` retains every constructed node so the Listener's `weak_ptr` stays valid for the lifetime of the subscription.
- `ClientSession.chains_[key]` stores the `keepAlive` vector. Cleared on `close()` / `handleCancel()` — that's what actually terminates the chain.

## 8. JSON protocols

### WebSocket (port `cfg.wsPort`, default 4000) — `ClientSession`

Subscribe:
```json
{"key": 1, "symbol": "AAPL", "field": "lastPrice", "pipeline": [{"type":"Worker","fn":"mean"}]}
```
- Integer `key` identifies the subscription (not a string `id`).
- `pipeline` overrides `node` if both present; pipeline is built in reverse order.
- `symbol` is the (neutral) stream key; `field` is the triggering event field.

Server replies:
```json
{"type": "subscribed", "key": 1}
{"type": "update",     "key": 1, "symbol": "AAPL", "value": 187.34}
```

The JSON `symbol` field name was kept deliberately (backwards compat) even though the engine is stream-neutral.

### TCP feed (port `cfg.feedPort`, default 9001) — `FeedServer` (market connector)

Default tick shape (no explicit `type`, routed as market tick):
```json
{"symbol":"AAPL", "lastPrice":187.42, "volume":350, "bid":187.40, "ask":187.43}
```

Market-specific OB messages:
```json
{"type":"ob",       "action":"add|update|delete|trade|ticksize", ...}
{"type":"control",  "action":"reset", "symbol":"AAPL", "epoch":2}
```

The `ob`/`control` schema is market-connector-owned. A different connector's feed server would route differently.

## 9. Shutdown sequencing

`rt::ShutdownCoordinator` (local in `main`, not a global). Steps are registered with `(name, order, fn)`; `stop()` runs them in ascending order exactly once. Signal handler triggers `stop()` via a static pointer.

Typical order: `ws-stop-accept=5, metrics-stop=10, ws-close-sessions=40, ob-provider-clear=50, feed-stop=55, feed-ws-stop=56, asio-stop=60, pool-drain=80, pool-destroy=85`.

## 10. Tests

The entire test binary (`gma_tests`) is one executable; `ctest` runs it. Organization under `tests/`:

- `engine/` — engine registry unit tests
- `connectors/` — synthetic connector tests (proves routing invariant)
- `book/`, `ob/`, `feed/` — market connector
- `dispatch/`, `nodes/`, `treebuilder/`, `validation/`, `registry/` — engine pipeline
- `integration/`, `ws/`, `connection/` — end-to-end
- `test_bootstrap.cpp` — gtest Environment that registers builtins + installs the market tick-computer factory before any test runs

`DispatcherRoutesByEventType` in `tests/connectors/SyntheticConnectorTest.cpp` is the regression check for "synthetic and market computers must not interfere." If that fails, the type-routing contract is broken.

## 11. Known residuals

Minor architectural debts that are deliberately unresolved — none block adding new connectors:

- **`include/gma/SourceProfile.hpp` lives in engine, not market.** `util::Config` has a `sourceProfile` member (priceFields/volumeFields/bidFields/askFields), so moving it to market would cascade. Cosmetic only.
- **JSON wire type name `"SymbolSplit"`** was preserved for backward compatibility when the class was renamed to `GroupSplit`. The string literal is correct and intentional; don't "fix" it.
- **No CI check yet** for the zero-core-changes invariant. The synthetic connector + its test prove it works today, but nothing prevents a future commit from re-introducing an engine → connector dependency. Plan §6 sketches a grep-based check.
- **`EventComputerRegistry`, `IngressRegistry`, `ConfigNamespaceRegistry`** are scaffolded but not yet on the hot path (dispatcher uses the `DefaultComputerFactory` hook; config parsing is still monolithic). Wiring them up is a future simplification, not a correctness issue.

## 12. Key files

If you need to find something quickly:

| Goal | File |
|---|---|
| Add a new node type | `include/gma/engine/NodeTypeRegistry.hpp`, register in your connector's `registerWith` |
| Add a math function | `FunctionMap::instance().registerFunction(...)` in your connector or in `src/core/BuiltinFunctions.cpp` for engine-wide defaults |
| Understand a TA value | `connectors/market/src/MarketTA.cpp::computeAllAtomicValues` |
| Trace an incoming WS request | `src/server/ClientSession.cpp::handleSubscribe` → `src/core/TreeBuilder.cpp::buildForRequest` |
| Trace an incoming tick | `connectors/market/src/server/FeedServer.cpp::handleLine` → `src/core/Dispatcher.cpp::onTick` |
| Change shutdown order | `src/main.cpp` (engine steps) + `connectors/market/src/MarketConnector.cpp` (market steps) |
| Look at the boot flow | `src/main.cpp` top-to-bottom — it's ~160 lines and narrates every stage |
