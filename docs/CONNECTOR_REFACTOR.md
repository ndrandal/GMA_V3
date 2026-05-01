# GMA_V3 Engine / Connector Refactor

## Goal

Split GMA_V3 into a domain-agnostic **core engine** + pluggable **connectors**. The market-specific code (order book, technical indicators, market atomics, ITCH adapter) becomes one connector. New connectors must be addable with **zero (or near-zero) changes to the core**.

**Invariant:** adding a connector should touch only (a) the new connector's own files and (b) a single registration line in the composition root.

---

## 1. Current boundary map

Three labels: **ENGINE** (generic, stays in core), **CONNECTOR** (market-specific, moves out), **VIOLATION** (currently straddles the line).

### Engine (keep; mostly clean)

| Component | Path | Status |
|---|---|---|
| Node base interface | `include/gma/nodes/INode.hpp` | ENGINE — pure virtual, domain-free |
| Pipeline nodes | `include/gma/nodes/{Worker,Aggregate,Interval,Responder,AtomicAccessor}.hpp`, `SymbolSplit.hpp`, `Listener.hpp` | ENGINE, but see violations below |
| Atomic store | `include/gma/AtomicStore.hpp`, `src/core/AtomicStore.cpp` | ENGINE — keyed by `(string, string)`, domain-free |
| Function registry | `include/gma/FunctionMap.hpp`, `src/core/FunctionMap.cpp` | ENGINE — generic `double(vector<double>)` registry |
| Atomic provider registry | `include/gma/atomic/AtomicProviderRegistry.hpp` | ENGINE — namespace-prefix dispatch, already neutral |
| Tree builder | `include/gma/TreeBuilder.hpp`, `src/core/TreeBuilder.cpp` | ENGINE with violations |
| JSON validator | `include/gma/JsonValidator.hpp`, `src/core/JsonValidator.cpp` | ENGINE with violations |
| Thread pool / shutdown / logger / metrics / config base | `include/gma/rt/`, `include/gma/runtime/`, `include/gma/util/` | ENGINE |
| WS accept & session plumbing (Beast) | `include/gma/server/WebSocketServer.hpp`, `src/server/WebSocketServer.cpp` | ENGINE (transport) |
| Execution context | `include/gma/ExecutionContext.hpp` | ENGINE |
| Worker functions in `registerBuiltinFunctions` | `src/core/AtomicFunctions.cpp:262-585` | ENGINE — generic stats/math |

### Connector (market) — becomes `libgma_connector_market`

| Component | Path |
|---|---|
| Order book engine | `include/gma/book/*`, `src/book/*`, `include/gma/ob/*`, `src/ob/*` |
| TA indicator suite | `include/gma/ta/*`, `computeAllAtomicValues` + helpers in `src/core/AtomicFunctions.cpp:16-260` |
| ITCH feed adapter | `include/gma/feed/ItchAdapter.hpp`, `src/feed/ItchAdapter.cpp` |
| TCP `FeedServer` message schema (ob/control routing) | `src/server/FeedServer.cpp:131-305` |
| OB provider registration for `"ob"` namespace | `src/main.cpp:184-189` |
| `OrderBookManager` snapshot source wiring | `src/main.cpp:143-179` |

### Violations — core code that knows market stuff

| # | File:line | Problem |
|---|---|---|
| V1 | `src/core/MarketDispatcher.cpp:38-42` | Hard-coded market field names `"lastPrice"/"openPrice"/"highPrice"/"lowPrice"/"prevClose"/"vwap"/"volume"/"obv"/"volatility_rank"/"mean"/"median"` in the `_skipFields` initializer |
| V2 | `src/core/MarketDispatcher.cpp:147-290` | `updateSymbolHistory` runs full TA suite inline: extracts price/volume/bid/ask, calls `computeAllAtomicValues`, writes `bid/ask/spread/timestamp` atomics |
| V3 | `src/core/MarketDispatcher.cpp:222-240` | `_profile.taEnabled` lightweight fallback computes `openPrice/highPrice/lowPrice` directly |
| V4 | `include/gma/MarketDispatcher.hpp:75` | Holds `unordered_map<string, SymbolHistory>` where `SymbolHistory = deque<TickEntry{price, volume, bid, ask, ts}>` |
| V5 | `include/gma/SymbolHistory.hpp:9-15` | `TickEntry` is a market record living in a core-namespace header |
| V6 | `include/gma/SourceProfile.hpp:17-34` | Engine-named but market-flavored: `priceFields`, `volumeFields`, `bidFields`, `askFields`, `taEnabled` |
| V7 | `src/core/AtomicFunctions.cpp:42-260` | `computeAllAtomicValues` is market TA in a file whose second half is pure engine |
| V8 | `include/gma/AtomicFunctions.hpp:7-27` | Engine-looking header exposes market entry point and includes `SymbolHistory.hpp` |
| V9 | `src/core/TreeBuilder.cpp:121-258` | `fnFromName` hard-codes a short list before delegating to `FunctionMap` — duplicates what's already registered |
| V10 | `src/core/TreeBuilder.cpp:280-299`, `476-487`, `326-344` | Requires `"symbol"` as a top-level spec field; dispatcher keys on `(symbol, field)` |
| V11 | `include/gma/TreeBuilder.hpp:26` | `Deps` hard-codes `MarketDispatcher*` — engine builder depends on a market class |
| V12 | `include/gma/nodes/Listener.hpp:13,26,48`, `include/gma/nodes/SymbolSplit.hpp` | `Listener` is wired to `MarketDispatcher*` specifically |
| V13 | `include/gma/SymbolValue.hpp:39-41`, `include/gma/SymbolTick.hpp:8-11` | Named `Symbol*` but the shape is fully generic |
| V14 | `include/gma/nodes/SymbolSplit.hpp:13` | Node named `SymbolSplit` — domain leak only |
| V15 | `src/core/JsonValidator.cpp:11-14, 80` | `KNOWN_NODE_TYPES` is a hard-coded closed list; `stringFields[]` includes `"symbol"`, `"field"`, `"function"`, `"fn"` as fixed vocabulary |
| V16 | `src/server/ClientSession.cpp:326-513` | WS subscribe handler demands `"symbol"` and `"field"`, echoes them back in every update |
| V17 | `src/server/FeedServer.cpp:131-305` | TCP feed routing hard-codes `type=="ob"` / `type=="control"` with book-specific actions |
| V18 | `src/main.cpp:34-40, 143-189, 200, 238-245` | Composition root news-up `OrderBookManager`, registers `"ob"` namespace, constructs `ItchAdapter`, wires specific market globals |
| V19 | `include/gma/util/Config.hpp:22-93` | One flat struct with TA/feed/OB/server fields mixed |
| V20 | `include/gma/feed/FeedEvent.hpp:24-67` | Canonical `FeedEvent` variant bakes in `ObAddEvent`, `ObTradeEvent`, `ObTickSizeEvent`, `ObResetEvent` |

---

## 2. Target architecture

### Module split

```
libgma_engine                     (no knowledge of any domain)
├── nodes/                        INode, Worker, Aggregate, Interval, AtomicAccessor, Responder,
│                                  GroupSplit (renamed SymbolSplit)
├── runtime/                      ThreadPool, ShutdownCoordinator
├── core/
│   ├── AtomicStore               (StreamKey, Field) -> ArgType
│   ├── FunctionMap               already generic
│   ├── AtomicProviderRegistry    already generic
│   ├── Dispatcher                generic (stream-key, event-type) -> listeners, no TA
│   ├── EventTypeRegistry         (new)
│   ├── EventComputerRegistry     (new) — replaces hard-coded TA calls
│   ├── NodeTypeRegistry          (new) — replaces closed KNOWN_NODE_TYPES
│   ├── ConnectorRegistry         (new) — the connector contract
│   ├── TreeBuilder               generic; no MarketDispatcher in Deps
│   └── JsonValidator             open vocabulary
├── util/                         Config (engine keys only), Logger, Metrics
├── transport/                    WebSocketServer + ClientSession (neutral protocol)
└── config/                       ConfigNamespaceRegistry (connectors own sub-namespaces)

libgma_connector_market
├── book/                         OrderBook, OrderBookManager
├── ob/                           Provider, FunctionalSnapshotSource, ObMaterializer, ObKey
├── ta/                           SMA/EMA/RSI/MACD/Bollinger/ATR/OBV/VWAP
├── feed/
│   ├── IFeedAdapter              (market concept; engine uses IngressAdapter)
│   ├── ItchAdapter
│   └── MarketFeedServer          (today's TCP feed server)
├── dispatcher/
│   ├── MarketEventComputer       owns TA; replaces Dispatcher::updateSymbolHistory
│   └── SymbolHistoryStore        TickEntry lives here
├── config/MarketConfig           TA periods, SourceProfile, OB allowNegativePrices
├── types/                        TickEntry, MarketFieldMap (née SourceProfile)
└── MarketConnector.cpp           registration entry point

gma_server                        composition root
└── main.cpp                      instantiates engine; calls MarketConnector::registerWith(...)
```

### Composition root

The composition root drives every connector through the same lifecycle:
construct → `registerWith` → `start` → … → `stop` (in reverse-registration
order via a single `connectors-stop` ShutdownCoordinator step at priority 30).
Connectors do NOT register their own ShutdownCoordinator steps.

```cpp
int main(int argc, char* argv[]) {
  // ... engine bootstrap (config, threadpool, store, dispatcher, ioc) ...
  gma::engine::EngineRegistries regs{ /* 14 fields, see EngineRegistries.hpp */ };

  std::vector<gma::engine::IConnector*> connectors;
  gma::market::MarketConnector marketConnector;
  marketConnector.registerWith(regs);
  connectors.push_back(&marketConnector);
  // (future) gma::crypto::CoinbaseConnector{}.registerWith(regs);

  for (auto* c : connectors) c->start();
  shutdown.registerStep("connectors-stop", 30, [&connectors] {
    for (auto it = connectors.rbegin(); it != connectors.rend(); ++it) {
      (*it)->stop();
    }
  });

  ioc.run();           // event loop
  shutdown.stop();     // drains every priority, including connectors-stop
  return 0;
}
```

### Connector contract

```cpp
namespace gma::engine {

struct EngineRegistries {
  EventTypeRegistry&       events;
  EventComputerRegistry&   computers;
  NodeTypeRegistry&        nodes;
  AtomicProviderRegistry&  providers;
  FunctionMap&             functions;
  ConfigNamespaceRegistry& config;
  IngressRegistry&         ingress;
  ShutdownCoordinator&     shutdown;
  ThreadPool&              pool;
  AtomicStore&             store;
  Dispatcher&              dispatcher;
  util::Logger&            log;
};

class IConnector {
public:
  virtual ~IConnector() = default;
  virtual std::string_view name() const = 0;
  virtual void registerWith(EngineRegistries&) = 0;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

} // namespace gma::engine
```

Lifecycle:
1. Engine constructs registries + dispatcher.
2. Composition root calls each connector's `registerWith(registries)`.
3. Engine calls `Config::load()` — engine parses its keys; unknown namespaces dispatch to connectors via `ConfigNamespaceRegistry`.
4. Engine calls `connector->start()` in registration order.
5. WS / feed servers begin accepting.
6. On shutdown, `ShutdownCoordinator` drains steps.

**Guarantee:** no connector writes to any static global in the engine. No engine file imports any connector header.

---

## 3. Extension points the engine must expose

### 3.1 `EventTypeRegistry`

`string -> EventSchema`. Populated during registration phase. `EventSchema` carries event name, listable field names (for validator), dispatch participation.

### 3.2 `EventComputerRegistry` — **replaces hard-coded TA in dispatcher**

```cpp
class IEventComputer {
public:
  virtual ~IEventComputer() = default;
  virtual std::string_view eventType() const = 0;
  virtual void compute(const Event& e, ComputeContext& ctx) = 0;
};

struct ComputeContext {
  AtomicStore* store;
  Dispatcher*  dispatcher;   // for notifying listeners
  ThreadPool*  pool;
};
```

The market connector's `TickComputer` lifts the body of `MarketDispatcher::updateSymbolHistory` (MarketDispatcher.cpp:147-290) into its `compute()`. Dispatcher only routes.

### 3.3 `NodeTypeRegistry`

```cpp
using NodeBuilderFn = std::function<std::shared_ptr<INode>(
    const rapidjson::Value&, const std::string&, const Deps&,
    std::shared_ptr<INode>)>;

void registerNodeType(std::string name, NodeBuilderFn);
```

Engine registers its built-ins (`Listener, Worker, Aggregate, Interval, AtomicAccessor, GroupSplit, Chain`) at engine construction. `JsonValidator::validateNode` consults the registry.

### 3.4 `AtomicProviderRegistry` — keep as-is (already clean)

### 3.5 `FunctionMap` — keep as-is (already correct)

### 3.6 `IngressRegistry`

```cpp
class IIngressSource {
public:
  virtual ~IIngressSource() = default;
  virtual void start() = 0;
  virtual void stop() noexcept = 0;
};

using IngressFactory =
  std::function<std::unique_ptr<IIngressSource>(
    boost::asio::io_context&, Dispatcher&, const util::ConfigView&)>;

void registerIngress(std::string kind, IngressFactory);
```

Market connector registers `kind="market-tcp-feed"` (today's FeedServer), `kind="market-ws-feed"` (today's WsFeedClient + ItchAdapter). Engine reads `ingress.N.kind = ...` out of config and creates via factory.

### 3.7 `ConfigNamespaceRegistry`

```cpp
void registerConfigNamespace(std::string prefix,
                             std::function<void(std::string_view key,
                                                std::string_view value)> reader);
```

When parsing `source.priceFields=...`, engine routes to the market connector's reader. Unknown prefixes stay silent (forward compat).

### 3.8 Sub-service registration

`ShutdownCoordinator::registerStep()` is already generic. Connectors use it directly.

---

## 4. Decisions adopted

| # | Decision | Adopted |
|---|---|---|
| Q1 | Engine holds opaque `StreamKey = std::string`. Listeners subscribe `(streamKey, eventType\|fieldName) -> INode`. `SymbolValue` → `StreamValue`, `SymbolTick` → `Event`. | **Yes** |
| Q2 | Static linkage with self-registration. Dynamic plugins deferred until a concrete third-party requirement appears. | **Yes** |
| Q3 | Keep `"symbol"` field in WS protocol as the neutral stream-key name. No wire-format change; C++ types renamed. | **Yes** |
| Q4 | `SourceProfile` moves wholesale into market connector as `MarketFieldMap`. Engine has no notion of "price" or "volume". | **Yes** |
| Q5 | `SymbolSplit` → `GroupSplit`. | **Yes** |
| Q6 | Preserve current TA listener-less behavior — TA atomics write to store but don't notify listeners unless `AtomicAccessor` is used. | **Yes** |
| Q7 | `FeedEvent` variant moves to connector. Engine uses `{ string type; string streamKey; shared_ptr<Document> payload; }`. | **Yes** |

---

## 5. Migration sequence

Each step independently buildable + testable. `ctest --output-on-failure` must stay green at every step.

### Step 0 — inventory & freeze
- This document as the reference.
- Snapshot current ctest output as the regression gate.

### Step 1 — cosmetic renames, no behaviour change
- `SymbolValue` → `StreamValue` (typedef alias for backward compat).
- `SymbolTick` → `Event` (alias).
- `SymbolSplit` → `GroupSplit` (alias).
- **Tests:** all existing pass unchanged.

### Step 2 — introduce engine registries as empty scaffolding
- Add `EventTypeRegistry`, `EventComputerRegistry`, `NodeTypeRegistry`, `IngressRegistry`, `ConfigNamespaceRegistry` headers+cpp under `include/gma/engine/` + `src/engine/`.
- Wire nothing to them yet.
- **Tests:** add `tests/engine/RegistriesTest.cpp`.

### Step 3 — move generic worker functions out of `AtomicFunctions.cpp`
- Split file: `registerBuiltinFunctions()` → `src/core/BuiltinFunctions.cpp`. `computeAllAtomicValues` + helpers → `src/core/MarketTA.cpp` (temporary home; moves to connector in step 7).
- **Tests:** `tests/core/AtomicFunctionsTest.cpp` still passes.

### Step 4 — convert `TreeBuilder`'s hard-coded fn ladder to pure `FunctionMap` lookup
- Delete the `if (fn == "mean") ...` ladder at `src/core/TreeBuilder.cpp:121-258`. Always route through `FunctionMap`. Behaviour-preserving.
- **Tests:** `tests/treebuilder/TreeBuilderTest.cpp`, `CorpusTest.cpp` green.

### Step 5 — introduce `NodeTypeRegistry` and convert node construction
- Replace the `if (type=="Listener") ...` ladder in `TreeBuilder.cpp:272-423` with registry lookup.
- `JsonValidator::validateNode` asks the registry instead of a closed set.
- Engine registers its built-in node types at startup.
- **Tests:** add `tests/treebuilder/CustomNodeTypeTest.cpp`.

### Step 6 — generalize `Dispatcher` + introduce `IEventComputer`
- Rename `MarketDispatcher` → `Dispatcher`; move to `src/core/Dispatcher.cpp`.
- Remove TA (`updateSymbolHistory`, `_symbolHistories`, `_profile`, `_skipFields`) from `Dispatcher`.
- Create `MarketTickComputer` implementing `IEventComputer`; lift V1–V4 logic out of `MarketDispatcher.cpp:147-290`.
- `main.cpp` registers the computer before starting servers (still in main for this step).
- **Tests:** `tests/dispatch/DispatchTest.cpp` + add `tests/engine/DispatcherIsolationTest.cpp`.

### Step 7 — carve out `libgma_connector_market` as a separate CMake target
- Three targets: `gma_engine`, `gma_connector_market`, `gma_server`.
- Move `include/gma/book/*`, `include/gma/ob/*`, `src/book/*`, `src/ob/*`, `src/feed/ItchAdapter.cpp`, `include/gma/feed/ItchAdapter.hpp`, `include/gma/feed/FeedEvent.hpp`, `src/core/MarketTA.cpp`, `src/core/MarketTickComputer.cpp`, `include/gma/SymbolHistory.hpp`, `include/gma/SourceProfile.hpp` (renamed) into `connectors/market/`.
- CMake check: engine target cannot include connector headers.
- **Tests:** existing pass.

### Step 8 — formalize `IConnector` + `MarketConnector::registerWith`
- Gather everything from `main.cpp:117-189, 204-251` into `connectors/market/MarketConnector.cpp::registerWith`.
- `main.cpp` shrinks to the composition root pattern above.
- **Tests:** add `tests/integration/ConnectorBootTest.cpp`.

### Step 9 — generalize `FeedEvent`
- Move `FeedEvent` variant fully into connector as `gma::market::MarketFeedEvent`.
- Engine's `Event` becomes `{ string type; string streamKey; shared_ptr<rapidjson::Document> payload; }`.
- `IFeedAdapter` moves to connector namespace; engine defines only `IIngressSource`.
- **Tests:** `tests/feed/FeedEventDispatchTest.cpp` moves to `connectors/market/tests/`.

### Step 10 — ship a trivial demonstration connector
- `connectors/synthetic/SyntheticConnector.cpp`: event type `"synthetic.tick"`, a computer that emits a sine wave to `AtomicStore`, an ingress source that schedules itself on `Interval`. ~150 LOC.
- Proves the zero-core-changes invariant: connector's code is entirely under `connectors/synthetic/`; `main.cpp` gets one new line.
- **Tests:** add `tests/connectors/SyntheticConnectorTest.cpp`, `tests/integration/TwoConnectorsTest.cpp`.

### Step 11 — cleanup
- Drop backward-compat aliases from step 1 once clients are migrated.
- Delete `include/gma/MarketDispatcher.hpp`.
- Update `CLAUDE.md`.

---

## 6. Risk & test strategy

### Engine in isolation
- `tests/engine/` depends **only** on `gma_engine` target. CMake enforces: if a test accidentally uses `OrderBookManager`, link fails.
- A "null connector" used by tests that don't need real data.

### Proving "zero core changes to add a connector"
- `SyntheticConnector` (Step 10) is the proof.
- CI check: grep-fail if `connectors/synthetic/` references `gma::market` or `gma::ob`.
- CI check: when a commit adds only `connectors/<new>/` contents, `git diff --stat` against `src/` + `include/` must show **only** `src/main.cpp` with a ≤3-line delta.

### Dispatcher + TA correctness regression
- Existing `DispatchTest.cpp`, `SourceProfileTest.cpp`, `IndicatorsTest.cpp`, `AtomicFunctionsTest.cpp`, `FeedEventDispatchTest.cpp`, `CorpusTest.cpp` must pass after every step.
- Before step 6: golden-output test — feed N ticks through current `MarketDispatcher`, snapshot all `AtomicStore` contents, check byte-equal after the refactor.

### WS protocol stability
- WS contract test: a fixture that sends exactly the JSON a current client sends and asserts the current JSON comes back. Locks the wire format before any rename work.

### Binary/ABI concerns
- Not applicable under static-linkage decision (Q2).
- If dynamic is chosen later, wrap registration entry point in `extern "C"` with POD-only arguments; all C++ types behind pimpl.

---

## Appendix A — Stale-memory corrections discovered during audit

- Runtime config file is `src/util/gma.conf` (INI), not `gma.json` (the latter only a conceptual schema).
- `src/ta/` directory is empty on disk. Live TA code is in `src/core/AtomicFunctions.cpp:16-260`. `include/gma/ta/Indicators.hpp` + `AtomicNames.hpp` are header-only helpers not called anywhere in the runtime path.
- Default SMA is `{5,20}` per `Config.hpp:29-30`, not `{5,10,20,50}`. The large set only applies if `taSMA` is explicitly set in `gma.conf`.
