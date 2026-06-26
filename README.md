# GMA_V3

**Version:** 3.0
**Author:** Nicholas Randall
**License:** Proprietary (All rights reserved) – see [LICENSE](./LICENSE)

---

## Overview

**GMA_V3** is a high-performance **C++20** WebSocket server and library for
real-time, atomic market-analysis computations over streaming data. Clients
connect over WebSocket, submit JSON-encoded **request trees**, and the node
pipeline runs nested statistical / technical-indicator math on the live feed,
pushing results back as values change.

The codebase is split into a **domain-agnostic core engine** plus **pluggable
connectors**. The engine knows nothing about markets; the market connector
contributes order-book and technical-analysis capabilities. Adding a new data
source (crypto, FIX, sensor feed, …) means writing a new connector — no engine
changes.

```
  data feed ──▶ ingress (FeedServer / WsFeedClient + adapter)
                     │  Event{ streamKey, payload, type="tick" }
                     ▼
              Dispatcher ──▶ MarketTickComputer ──▶ AtomicStore / OrderBook
                     │
                     ▼
   WS clients ──▶ ClientSession ──▶ TreeBuilder ──▶ node pipeline ──▶ Responder ──▶ WS
```

> For the deeper picture (event lifecycle, connector contract, registries, how
> to add a connector), read **[CLAUDE.md](./CLAUDE.md)** and
> **[docs/ARCHITECTURE.md](./docs/ARCHITECTURE.md)** — those are the
> authoritative references; this README is the quick start.

Key properties:

- **Atomic computation model** — each operation (`mean`, `rsi_14`, `sma_5`, an
  `AtomicAccessor` on an `ob.*` key, …) is an independent node, enabling
  fine-grained pipelining and reuse.
- **Composable request trees** — clients compose nested pipelines of nodes
  (Listener → Worker/Aggregate/AtomicAccessor → Responder).
- **Engine / connector split** — `libgma_engine` is source-agnostic;
  `libgma_connector_market` adds OB + TA + feed adapters. CMake include-scoping
  enforces that the engine never depends on connector headers.
- **WebSocket interface** — clients connect, subscribe, and receive continuous
  value updates.

---

## Repository structure

```
├── CMakeLists.txt              # Root build (engine + connectors + server + tests)
├── LICENSE                     # Proprietary, all-rights-reserved licence
├── include/gma/                # Engine public headers (libgma_engine)
│   ├── engine/                 # IConnector, EngineRegistries, registries
│   ├── nodes/                  # INode, Listener, Worker, Aggregate, Interval, ...
│   ├── server/                 # WebSocketServer, ClientSession, RequestKey
│   ├── ws/                     # WsBridge, WSResponder
│   ├── rt/                     # ThreadPool, SPSCQueue
│   ├── runtime/                # ShutdownCoordinator
│   ├── util/                   # Config, Logger, Metrics
│   ├── Dispatcher.hpp  Event.hpp  AtomicStore.hpp  FunctionMap.hpp  TreeBuilder.hpp
│   └── ...
├── src/                        # Engine implementations (mirrors include/)
│   ├── core/                   # Dispatcher, TreeBuilder, Expr, builtins
│   ├── nodes/  server/  ws/  rt/  util/
│   ├── util/gma.conf           # Example INI runtime config
│   └── main.cpp                # Composition root — boots engine, registers connectors
├── connectors/
│   ├── market/                 # libgma_connector_market (OB, TA, ITCH, FeedServer, WsFeedClient)
│   │   ├── include/gma/        # market/, book/, ob/, ta/, feed/, ws/, server/
│   │   └── src/
│   └── synthetic/              # demo connector (linked into tests only)
├── tests/                      # GoogleTest suites (one gma_tests binary)
└── docs/                       # ARCHITECTURE.md, atomic-keys.md, feed-adapters.md, ...
```

---

## Prerequisites

- **CMake ≥ 3.20**
- **C++20 compiler** (GCC 12+, Clang 14+ — clang++ is preferred by `tools/compile.sh`)
- **Boost.Asio / Beast** — networking + WebSocket
- **RapidJSON** — JSON parsing
- **GoogleTest** (only when `GMA_BUILD_TESTS=ON`)
- **Python 3** (utility scripts)

Third-party deps are vendored under `third_party/` (override with
`-DTHIRD_PARTY_ROOT=...`).

---

## Build

```bash
# Release
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Debug + tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DGMA_BUILD_TESTS=ON
cmake --build build -j$(nproc)

# Or use the helper (prefers clang++)
./tools/compile.sh
```

CMake targets: `libgma_engine.a`, `libgma_connector_market.a`,
`libgma_connector_synthetic.a`, the **`gma_server`** binary, and `gma_tests`.

---

## Run

```bash
# Compiled defaults: wsPort=8080, feedPort=9001 (no config file loaded)
./build/gma_server

# argv[1]=wsPort, argv[2]=configFile, argv[3]=feedPort (argv wins over the file)
./build/gma_server 9002
./build/gma_server 9002 gma.conf        # load the example INI (it sets wsPort=4000,
                                        #   but argv[1]=9002 overrides it)
./build/gma_server 9002 gma.conf 9005   # also override feedPort
```

> **Port note:** the **compiled-in** default `wsPort` is **8080**
> (`include/gma/util/Config.hpp`). The shipped `src/util/gma.conf` *overrides*
> it to **4000**, so the port you get depends on whether you pass that file.

### Configuration

Runtime config is an **INI `key=value` file** (see `src/util/gma.conf`) parsed by
`src/util/Config.cpp` — there is no compile-time config struct to edit. Keys
include `wsPort`, `feedPort`, `threadPoolSize`, `logLevel`,
`metricsEnabled`/`metricsIntervalSec`, the TA periods
(`taSMA`, `taEMA`, `taRSI`, `taATR`, `taMomentum`, `taVolAvg`, `taMACD_*`,
`taBBands_*`), memory bounds (`taHistoryMax`, `maxSymbols`,
`maxFieldsPerSymbol`), `allowNegativePrices`, the `ingress.N.*` ingress entries,
and the `market.source.*` field-map namespace. See **[CLAUDE.md](./CLAUDE.md) →
Configuration** for the full table (including compiled-default vs `gma.conf`
values) and **[docs/feed-adapters.md](./docs/feed-adapters.md)** for the source
field map.

---

## WebSocket protocol

Clients send a `subscribe` message whose `requests` array each carries a
**`streamKey`** (string, required — there is **no `symbol` alias**, ENC-50), a
**`field`** to trigger on, a per-request id (`key` int **or** `id` int|string,
not both), and an optional `pipeline`/`node` sub-tree.

```jsonc
// int-keyed subscribe
{ "type": "subscribe", "requests": [
  { "key": 1, "streamKey": "NEXO", "field": "lastPrice",
    "pipeline": [ { "type": "Worker", "fn": "mean" } ] }
] }
// → { "type": "subscribed", "key": 1 }
// → { "type": "update", "key": 1, "streamKey": "NEXO", "value": 24.83 }
```

```jsonc
// string-id subscribe (mirrored back as "requestId")
{ "type": "subscribe", "requests": [
  { "id": "r-NEXO-open", "streamKey": "NEXO", "field": "lastPrice" }
] }
// → { "type": "update", "requestId": "r-NEXO-open", "streamKey": "NEXO", "value": 24.83 }
```

Errors come back as `{"type":"error","where":"<stage>","message":"..."}` where
`<stage>` is `parse` / `type` / `subscribe` / `validate` / `build`.

Two atomic-key namespaces exist — **bare** keys (`lastPrice`, `bid`, `sma_5`,
`rsi_14`, `macd_line`, `bollinger_upper`, …) are Listener-subscribable, while
**`ob.*`** keys (`ob.best.bid.price`, `ob.spread`, …) are **pipeline-only** (read
via an `AtomicAccessor`, never a direct Listener). The push-vs-pull rule, the
canonical `ob.*`-in-a-chart pattern, and the exact emitted key names are in
**[docs/atomic-keys.md](./docs/atomic-keys.md)**.

### TCP feed (market connector, default port 9001)

The market connector's `FeedServer` accepts GMA's own line-delimited JSON:

```jsonc
{"symbol":"AAPL","lastPrice":187.42,"volume":350,"bid":187.40,"ask":187.43}
{"type":"ob","action":"add","symbol":"AAPL","id":1,"side":"bid","price":187.40,"size":100}
{"type":"control","action":"reset","symbol":"AAPL"}
```

External vendor feeds (e.g. NASDAQ ITCH) are ingested over `WsFeedClient` + a
feed adapter — see **[docs/feed-adapters.md](./docs/feed-adapters.md)** and
**[docs/writing-adapters.md](./docs/writing-adapters.md)**.

---

## Testing

```bash
cmake -B build -DGMA_BUILD_TESTS=ON && cmake --build build -j$(nproc)
cd build && ctest --output-on-failure

# A single suite
./gma_tests --gtest_filter="ItchAdapterTest.*"
```

All tests build into one `gma_tests` executable.

---

## License & Contact

This code is **proprietary**:

> Copyright © 2025 Nicholas Randall
> All rights reserved.
> Personal, non-commercial viewing only; all other rights reserved.

For licensing inquiries, contact **Nicholas Randall**.
