# CLAUDE.md — GMA_V3

## Project Overview

GMA_V3 is a high-performance C++20 WebSocket server and library for real-time atomic analysis computations over streaming data. Clients submit JSON-encoded request trees that execute nested statistical operations asynchronously.

The codebase is organized as a **domain-agnostic core engine** plus **pluggable connectors**. The market connector contributes order-book / technical-analysis capabilities; adding a new data source (crypto, FIX, sensor feed, …) means writing a new connector with no changes to the engine.

For the deeper architectural picture, see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Build & Run

```bash
# Build (Release)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)

# Build with tests
cmake .. -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON
cmake --build . -j$(nproc)

# Build via script (prefers clang++)
./tools/compile.sh

# Run server
./build/gma_server                 # compiled defaults (wsPort=8080, feedPort=9001)
./build/gma_server 9002            # custom wsPort
./build/gma_server 9002 gma.conf   # custom wsPort + INI config (gma.conf sets wsPort=4000,
                                   #   but argv[1]=9002 wins over the file)
./build/gma_server 9002 gma.conf 9005  # also override feedPort

# Run tests
cd build && ctest --output-on-failure
```

## Project Structure

```
include/gma/              # Engine public headers (libgma_engine)
  engine/                 # Engine-side contracts & registries
                          #   IConnector, EngineRegistries, IEventComputer,
                          #   EventTypeRegistry, EventComputerRegistry,
                          #   NodeTypeRegistry, IngressRegistry,
                          #   ConfigNamespaceRegistry
  nodes/                  # INode, Listener, Worker, Aggregate, Interval,
                          # AtomicAccessor, Responder, GroupSplit
  server/                 # WebSocketServer, ClientSession
  ws/                     # WebSocket bridge/responder
  rt/                     # ThreadPool, SPSCQueue
  runtime/                # ShutdownCoordinator
  util/                   # Config, Logger, Metrics
  atomic/                 # AtomicProviderRegistry
  Dispatcher.hpp          # Generic event-routing hub
  Event.hpp               # Canonical {type, symbol, payload} event (the `symbol`
                          # field is an opaque streamKey internally; WS payloads
                          # use "streamKey" as the JSON key — ENC-50)
  StreamValue.hpp         # ArgType + pipeline value
  AtomicStore.hpp         # Thread-safe (symbol, field) -> ArgType store
  FunctionMap.hpp         # Named worker-function registry
  FunctionRegistry.hpp    # registerBuiltinFunctions()
  NodeRegistry.hpp        # registerBuiltinNodeTypes()
  TreeBuilder.hpp         # JSON → node DAG
  # (NOTE: there is no engine-level SourceProfile.hpp — field-alias mapping
  #  moved into the market connector as gma::market::MarketFieldMap (ENC-35);
  #  see connectors/market/include/gma/market/MarketFieldMap.hpp)

src/                      # Engine implementations (mirrors include/ layout)
  main.cpp                # Composition root — boots engine, wires connectors

connectors/market/        # libgma_connector_market (depends on gma_engine)
  include/gma/
    market/MarketConnector.hpp
    book/ ob/ ta/         # OB engine, TA indicator suite
    feed/                 # IFeedAdapter, ItchAdapter, FeedEvent
    server/FeedServer.hpp # TCP feed server
    ws/WsFeedClient.hpp
    MarketTA.hpp          # MarketTickComputer + computeAllAtomicValues
    SymbolHistory.hpp     # TickEntry
    AtomicFunctions.hpp   # Transitional umbrella (TA + builtins)
  src/                    # Implementations mirrored

connectors/synthetic/     # libgma_connector_synthetic (demo; linked only into tests)
  include/gma/synthetic/SyntheticConnector.hpp
  src/SyntheticConnector.cpp

tests/                    # GoogleTest suites
  engine/                 # Registry tests
  connectors/             # SyntheticConnectorTest
  (book/ dispatch/ feed/ integration/ nodes/ ob/ registry/ treebuilder/
   validation/ ws/ — domain tests)
  test_bootstrap.cpp      # Global gtest Environment — installs builtins
                          # + MarketConnector default computer factory

tools/                    # Python/shell utilities (compile.sh, mapping, todo_scan)
docs/
  ARCHITECTURE.md         # Deeper architecture reference
  CONNECTOR_REFACTOR.md   # Engine/connector split — historical plan
```

## Key Architecture (1-paragraph orientation)

A connector registers itself at boot via `MyConnector::registerWith(EngineRegistries&)`. The `Dispatcher` routes inbound `Event`s by their `type` field to per-dispatcher `IEventComputer` instances (supplied by connectors). Listeners subscribe on `(streamKey, field)` and receive `StreamValue`s from direct event fields (via `Dispatcher`) or from computer-written atomics (via `Dispatcher::notifyListeners`). JSON request trees are built through `TreeBuilder`, which looks up node constructors in `NodeTypeRegistry`; worker math names resolve via `FunctionMap`. The wire-format JSON key is `streamKey` everywhere — no `symbol` alias is accepted (ENC-50).

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full picture including the event lifecycle, connector contract, and a step-by-step guide for adding a new connector.

## Tech Stack

- **C++20** (`-DCMAKE_CXX_STANDARD=20`)
- **CMake 3.20+** — produces `libgma_engine.a`, `libgma_connector_market.a`, `libgma_connector_synthetic.a`, `gma_server`, `gma_tests`
- **Boost.Asio / Beast** — networking + WebSocket
- **RapidJSON** — JSON parsing and validation
- **GoogleTest** — unit + integration tests

## Code Conventions

- Engine code under `include/gma/` + `src/`; connector code under `connectors/<name>/`
- Engine CMake target (`gma_engine`) must not depend on any connector headers
- Namespace: `gma::` for public types, `gma::engine::` for engine contracts, `gma::market::` / `gma::synthetic::` for connector code
- Globals: `gma::gThreadPool` (shared_ptr<ThreadPool>, set up in main.cpp)
- Runtime config: INI-style key=value files (see `src/util/gma.conf`)
- Prefer lock-free / fine-grained locking (shared_mutex, SPSCQueue)
- Test files named `<Component>Test.cpp` under `tests/<category>/`; the gtest binary is a single `gma_tests` executable
- Connectors implement the strict `IConnector` lifecycle: `registerWith()` allocates and registers (no live sockets/timers), `start()` brings sources online, `stop()` noexcept tears down in reverse order. The composition root drives all three; never wire your own `ShutdownCoordinator` step from inside a connector.
- **Ingress sources are engine-owned (ENC-31).** Connectors register named factories on `reg.ingress` (e.g. `market.feedserver`, `market.wsclient`); the composition root reads `cfg.ingress[]` and instantiates them. Adding a new ingress kind is a factory registration + INI edit, not a `main.cpp` change. Legacy `feedPort` / `feedUrl` / `feeds.N.*` keys are auto-translated into `cfg.ingress[]` entries with a one-release deprecation warn.
- **WS request keys — int vs string (`RequestKey`).** The subscribe / cancel / value-emit code paths in `ClientSession` and `Responder` use `gma::server::RequestKey = std::variant<int, std::string>` (header at `include/gma/server/RequestKey.hpp`). Inbound subscribe accepts `{key:<int>}`, `{id:<int>}` (legacy), or `{id:"<string>"}`; outbound frames render `"key":<int>` or `"requestId":"<string>"` to mirror. Engine internals (Dispatcher, AtomicStore, TreeBuilder, Listener) stay key-type-agnostic — they route on `(streamKey, field)`, not on the request id. See [`docs/atomic-keys.md`](docs/atomic-keys.md) §"Subscribe request key — int vs string".
- **Atomic-key namespaces — bare vs `ob.*` (ENC-94, ENC-101).** Two distinct namespaces by source: bare (`bid`, `ask`, `lastPrice`, sma_N, ...) is written by `MarketTickComputer` only when the tick payload carries the field directly (pre-aggregated tick connectors). `ob.*` (`ob.best.bid.price`, `ob.spread`, ...) is computed from `OrderBookManager` state — used for L2/L3 sources (ITCH, FIX). **Listeners may bind only to bare keys; `ob.*` is pipeline-only** — `ob::Provider` never calls `Dispatcher::notifyListeners`, so a `Listener` bound to an `ob.*` field would silently never fire. The reject lives in `nodes::Listener::Create` (the static factory; the public constructor is kept for unit-test fixtures only) and surfaces as a `{"type":"error","where":"build","message":"listener: field '...' is pipeline-only — see docs/atomic-keys.md..."}` WS frame. Canonical pattern for surfacing `ob.*` into a chart: `Listener(<bare-key clock>) → AtomicAccessor(ob.*) → ...` — see [`docs/atomic-keys.md`](docs/atomic-keys.md) for the worked NEXO example.

## Configuration

Two layers, and they intentionally differ — keep them straight:

1. **Compiled-in defaults** (`include/gma/util/Config.hpp`) — what
   `./gma_server` uses when **no** config file is passed.
2. **Shipped INI** (`src/util/gma.conf`) — an example config; it overrides some
   compiled defaults when you actually pass it as `argv[2]`.

| Key | Compiled default (`Config.hpp`) | `gma.conf` value |
|---|---|---|
| `wsPort` | **8080** | 4000 |
| `feedPort` | 9001 | 9001 |
| `threadPoolSize` | 0 (= hardware_concurrency) | 4 |
| `taSMA` (SMA periods) | `{5, 20}` | `5,10,20,50` |
| `taEMA` (EMA periods) | `{12, 26}` | `9,21,50` |
| `taRSI` | 14 | 14 |
| `taATR` | 14 | 14 |
| `taMomentum` (momentum/ROC) | 10 | 10 |
| `taVolAvg` (volume avg) | 20 | 20 |
| `taMACD_fast` / `taMACD_slow` / `taMACD_signal` | 12 / 26 / 9 | 12 / 26 / 9 |
| `taBBands_n` / `taBBands_stdK` | 20 / 2.0 | 20 / 2.0 |
| `taHistoryMax` | 1000 | 1000 |
| `metricsEnabled` / `metricsIntervalSec` | false / 15 | true / 15 |
| `logLevel` | `info` | `info` |

> ⚠️ The compiled SMA/EMA periods (`{5,20}` / `{12,26}`) are **not** what
> `gma.conf` ships (`5,10,20,50` / `9,21,50`). Whichever you load wins; don't
> assume one from the other.

Other engine keys parsed by `src/util/Config.cpp` (defaults from `Config.hpp`):

- **Memory bounds:** `maxSymbols` (10000), `maxFieldsPerSymbol` (200).
- **Order book:** `allowNegativePrices` (false) — allow negative prices/yields.
- **Ingress (ENC-31, the current model):** `ingress.N.kind` plus per-entry
  sub-keys (`ingress.N.port`, `ingress.N.url`, `ingress.N.adapter`,
  `ingress.N.symbols`, …). The engine instantiates each entry by kind
  (`market.feedserver`, `market.wsclient`).
- **Legacy feed keys (auto-translated into `ingress[]` with a deprecation
  warn):** `feedUrl`, `feedSymbols`, and `feed.N.{url,adapter,symbols}`.
- **forum-driven ingress:** `forumUrl`, `forumTenantId`, `forumAuthToken` —
  when `forumUrl` is set, ingress is pulled from forum instead of the static
  INI list.
- **Connector-namespaced config** (handled by `ConfigNamespaceRegistry`, not the
  engine directly): `market.source.*` → `MarketFieldMap` (legacy bare `source.*`
  is a one-release alias). See `docs/feed-adapters.md`.

CLI override order: `argv[1]=wsPort`, `argv[2]=configFile`, `argv[3]=feedPort`.
Values from argv win over the config file; the config file wins over the
compiled defaults.
