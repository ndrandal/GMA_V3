# GMA Smoke Test

End-to-end smoke test that connects to the GMA WebSocket server, subscribes to
all pipeline patterns, and verifies data flows through the full stack.

## Prerequisites

- Node.js 18+
- dependencies installed with `npm ci` (run from this directory)
- GMA server running and receiving feed data

## Install

```bash
cd tools/smoke-test
npm ci
```

`node_modules/` is **not** tracked in git (ENC-878); `npm ci` installs the exact
versions pinned in `package-lock.json`.

## Usage

```bash
node smoke.js [--url ws://localhost:8080] [--duration 30] [--verbose]
```

| Flag         | Default                | Description                          |
|--------------|------------------------|--------------------------------------|
| `--url`      | `ws://localhost:8080`  | GMA WebSocket server URL             |
| `--duration` | `30`                   | Seconds to collect updates           |
| `--verbose`  | off                    | Log every individual update received |

## What It Tests

The script subscribes to ~20 keys across 6 tiers:

| Tier | Pattern          | Keys   | What it validates                        |
|------|------------------|--------|------------------------------------------|
| 1    | Direct field     | 1-10   | Raw tick + TA fields flow to clients     |
| 2    | Worker pipeline  | 20-23  | FunctionMap computations (mean, max, etc)|
| 3    | AtomicAccessor   | 30-32  | Store reads (TA, order book)             |
| 4    | Interval         | 40-41  | Timer-driven polling                     |
| 5    | Multi-symbol     | 50-52  | Independent symbol routing               |
| 6    | Aggregate        | 60     | Batched N-tick aggregation               |

## Launch Sequence

### Option A: Local feed simulator

```bash
# Terminal 1 — feed simulator
cd ~/Github/feed-simulator/go-feed && go run ./cmd/feedsim -port 8100

# Terminal 2 — GMA server
cd ~/Github/GMA_V3
GMA_FEED_URL=ws://localhost:8100/feed ./build/gma_server 8080

# Terminal 3 — smoke test
cd ~/Github/GMA_V3/tools/smoke-test
node smoke.js --url ws://localhost:8080 --duration 30 --verbose
```

### Option B: Public feed

```bash
GMA_FEED_URL=wss://feed-sim.v3m.xyz/feed ./build/gma_server 8080
node tools/smoke-test/smoke.js --url ws://localhost:8080 --duration 30
```

## Expected Output

```
[smoke] Connected to ws://localhost:8080
[smoke] Sent 20 subscribe requests
...
[smoke] === RESULTS ===

  Key    Subscription                        Updates  First (s)
  ---------------------------------------------------------------
  1      NEXO/lastPrice                           47        0.3
  3      NEXO/sma_5                               47        1.2
  6      NEXO/rsi_14                              35        3.5
  20     NEXO/Worker(mean)                        47        0.4
  31     NEXO/Atomic(ob.spread)                   47        0.5
  40     NEXO/Interval(sma_5)                     30        1.1
  ...

[smoke] === SUMMARY ===
[smoke] Duration: 30s | Total updates: 842 | Keys with data: 20/20
[smoke] PASS
```

## Exit Codes

- **0** — All keys received at least one update (PASS)
- **1** — One or more keys received no updates (FAIL)

## Run logs

Run output is **not** committed. A checked-in log of one past run cannot be kept true
by anything — there is no CI to regenerate it (server-side CI was decided against
workspace-wide; the local pre-push gate is the mechanism), so it silently goes stale the
next time `smoke.js` changes and then misleads the next reader. That is exactly what
happened to the old `SMOKE_RUN_LOG.md`, deleted in ENC-1000: written 2026-03-20, never
updated, still advertising symbols and fields (`BLITZ`, `ema_12`) that `smoke.js` stopped
using in 2026-05.

Run the script and read its own output; the shape to expect is under **Expected Output**
above. The old log is recoverable from git history if you ever want it (`git log --
tools/smoke-test/SMOKE_RUN_LOG.md`).
