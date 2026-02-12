# CLAUDE.md — GMA_V3

## Project Overview

GMA_V3 is a high-performance C++20 WebSocket server and library for real-time atomic market-analysis computations. Clients submit JSON-encoded request trees that execute nested statistical and technical-indicator operations asynchronously.

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
./build/gma_server              # default port 8080
./build/gma_server 9002         # custom port
./build/gma_server 9002 cfg.ini # custom port + config

# Run tests
cd build && ctest --output-on-failure
```

## Project Structure

```
include/gma/          # Public API headers
  nodes/              # Node pipeline (INode, Listener, Worker, Responder, Aggregate, etc.)
  ta/                 # Technical analysis (SMA, EMA, RSI, VWAP, Bollinger Bands, MACD)
  ob/                 # Order book engine
  book/               # Order book types
  server/             # WebSocket server (Boost.Beast)
  ws/                 # WebSocket bridge/responder layer
  rt/                 # Runtime (ThreadPool, SPSCQueue)
  runtime/            # Shutdown coordination
  util/               # Config, Logger, Metrics
  atomic/             # Atomic provider registry
src/                  # Implementation files (mirrors include/ layout)
  main.cpp            # Entry point — signal handlers, config, server startup
tests/                # GoogleTest suites (core, nodes, integration, dispatch, etc.)
tools/                # Python/shell utility scripts (compile.sh, mapping tools, todo_scan)
```

## Key Architecture

- **Node Pipeline**: Computation is expressed as linked nodes (Listener → Worker → Responder). TreeBuilder converts JSON requests into DAGs.
- **MarketDispatcher**: Routes market ticks to listeners, maintains symbol history, triggers atomic computations.
- **AtomicStore**: Thread-safe key-value store for computed results.
- **FunctionMap**: Singleton registry of available atomic functions.
- **ThreadPool**: Global thread pool (`gma::gThreadPool`), default 4 threads.
- **ShutdownCoordinator**: `gma::gShutdown` for graceful sequenced teardown.

## Tech Stack

- **C++17** (CMake configured) / C++20 features used in code
- **CMake 3.20+**
- **Boost.Asio / Beast** — networking and WebSocket
- **RapidJSON** — JSON parsing and validation
- **GoogleTest** — unit and integration tests

## Code Conventions

- Headers in `include/gma/`, implementations in `src/` (mirrored subdirectory layout)
- Namespace: `gma` (sub-namespaces: `gma::nodes`, `gma::ta`, `gma::ob`, etc.)
- Global singletons: `gma::gThreadPool`, `gma::gShutdown`
- Config: INI-style key=value files; runtime config via `gma.json`
- Lock-free structures preferred (ring buffers, SPSCQueue)
- Test files named `<Component>Test.cpp` in category subdirectories under `tests/`

## Configuration

Default runtime config at `src/util/gma.json`:
- WebSocket port: 4000 (CLI override available)
- Thread pool: 4 threads
- Listener queue capacity: 1024
- TA defaults: SMA [5,10,20,50], EMA [9,21,50], RSI 14, MACD [12,26], BBands [20,2]
