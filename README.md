# GMA_V3

**Version:** 3.0  
**Author:** Nicholas Randall  
**License:** Proprietary (All rights reserved) – see [LICENSE](./LICENSE)

---

## Overview

**GMA_V3** is a high-performance C++20 library and accompanying WebSocket server designed for real-time, atomic market‐analysis computations. By accepting JSON‐encoded request trees, it executes nested statistical and technical‐indicator operations asynchronously and streams results back to clients with minimal latency.

Key highlights:

- **Atomic Computation Model**: Each operation (e.g., `mean`, `RSI`, `VWAP`) is encapsulated as an independent node, enabling fine-grained pipelining and reuse.  
- **Composable Request Trees**: Clients compose arbitrary nested trees of operations, allowing expressions such as "EMA of a 5‐period SMA" or "volatility rank of last 20 closes."  
- **Lock-Free Data Structures**: Internally uses ring buffers and wait-free queues to minimize contention under heavy load.  
- **Configurable Threading**: A dynamic thread pool balances throughput and resource usage, adapting to server load.  
- **WebSocket Interface**: Zero-configuration startup; clients connect over WS, send requests, and subscribe to continuous updates.

---

## Design Principles

1. **Separation of Concerns**  
   - _Core Engine vs. Transport_: The computation engine (tree builder, execution context, function map) is decoupled from the WebSocket transport. This clear boundary ensures maintainability and allows swapping in alternative transports (e.g., REST or gRPC) in the future.

2. **Single Responsibility**  
   - _Modular Nodes_: Each statistical or mathematical function is implemented in its own `Node` class. This adheres to the SRP from SOLID, making unit testing, extension, and replacement straightforward.

3. **Open/Closed Extensibility**  
   - _Pluggable FunctionMap_: New atomic functions can be registered at runtime without modifying existing code, thanks to a registry‐based design. This encourages plugin-style extensions for custom analytics.

4. **Performance-Driven Architecture**  
   - _Lock-Free Buffers_: History and dispatch queues leverage ring buffers to avoid blocking in hot code paths, ensuring sub‐microsecond enqueuing and dequeuing under contention.  
   - _Cache Locality_: Data structures are laid out to optimize CPU cache usage, minimizing pointer chasing during deep tree evaluation.

5. **Asynchronous & Reactive**  
   - _Event‐Driven Dispatch_: The engine reacts to incoming data updates, propagating changes through the computation graph and notifying clients only on actual value changes. This push model conserves CPU and network bandwidth.

6. **Configurability & Observability**  
   - _Runtime Settings_: Buffer sizes, thread counts, and log levels are all tunable via a single `Config.hpp`.  
   - _Structured Logging_: Built‐in logger supports hierarchical contexts and can integrate with external tracing systems for end‐to‐end observability.

7. **Robust JSON Validation**  
   - _Schema Enforcement_: Input requests are validated against a strict JSON schema, preventing malformed or malicious payloads from entering the core engine.

8. **Test-First Reliability**  
   - _Comprehensive GTest Suite_: Every module—from node implementations to dispatcher logic—is covered by unit tests. Integration tests simulate full request‐response cycles to guard against regressions.

---

## Repository Structure

```
├── CMakeLists.txt          # Root build configuration
├── LICENSE                 # Proprietary, all-rights-reserved licence
├── .gitignore              # Ignored files and directories
├── include/                # Public headers (API)  
│   └── gma/                # Namespace folder
│       ├── AtomicFunctions.hpp
│       ├── AtomicStore.hpp
│       ├── Config.hpp
│       ├── ExecutionContext.hpp
│       ├── FunctionMap.hpp
│       ├── JsonValidator.hpp
│       ├── Logger.hpp
│       ├── MarketDispatcher.hpp
│       ├── RequestRegistry.hpp
│       └── server/         # WebSocket server API headers
│           ├── WebSocketServer.hpp
│           └── ClientSession.hpp
├── src/                    # Implementation details  
│   ├── core/               # Engine components
│   ├── nodes/              # Specific function node logic
│   ├── server/             # Transport layer (WebSocket)
│   └── main.cpp            # Server entry point
└── tests/                  # GTest test suites (requires BUILD_TESTS=ON)
    ├── core/               # Core engine tests
    ├── nodes/              # Node behavior tests
    ├── integration/        # End-to-end scenarios
    └── ...
```

---

## Prerequisites & Dependencies

- **CMake ≥ 3.15**  
- **C++20‐compatible compiler** (GCC 10+, Clang 12+, MSVC 2019+)  
- **Boost.Asio** (header-only or compiled)  
- **GoogleTest** (optional; for tests)  
- **Python 3** (utility scripts)

Dependencies can be installed via OS package managers or built from source. The `THIRD_PARTY_DIR` CMake variable lets you specify custom library locations.

---

## Build & Installation

```bash
# Clone and enter
git clone https://github.com/yourusername/GMA_V3.git
cd GMA_V3

# Create build directory
mkdir build && cd build

# Configure (Release + Tests)
cmake ..   -DCMAKE_BUILD_TYPE=Release   -DBoost_DIR=/path/to/boost   -DBUILD_TESTS=ON

# Build
cmake --build . -- -j$(nproc)
```

Artifacts:
- `gma-server` (WebSocket server)
- Test binaries under `tests/`

---

## Usage Example

```bash
# Start server (defaults to port 9002)
./gma-server
```

Client JSON sample:

```json
{
  "clientId": "trader-42",
  "requests": [
    {
      "seriesId": "AAPL",
      "operations": [
        { "type": "lastPrice" },
        { "type": "mean", "period": 10 }
      ]
    }
  ]
}
```

Server pushes updates whenever computed values change, supporting thousands of concurrent streams with minimal overhead.

---

## Configuration

Modify `include/gma/Config.hpp` to tune:

```cpp
struct Config {
  static constexpr size_t ListenerQueueMax   = 1000;
  static constexpr size_t HistoryMaxSize     = 2048;
  static constexpr size_t ThreadPoolSize     =    8;
};
```

Adjusting these parameters lets you balance memory, latency, and throughput for your deployment environment.

---

## Testing & Validation

```bash
# From build dir
ctest --output-on-failure
```

Each test suite outputs detailed diagnostics on failure, ensuring new contributions maintain correctness and performance guarantees.

---

## License & Contact

This code is **proprietary**:

> Copyright © 2025 Nicholas Randall  
> All rights reserved.  
> Personal, non-commercial viewing only; all other rights reserved.

For licensing inquiries or commercial agreements, contact **Nicholas Randall** at <your.email@example.com> or via GitHub [yourusername](https://github.com/yourusername).
