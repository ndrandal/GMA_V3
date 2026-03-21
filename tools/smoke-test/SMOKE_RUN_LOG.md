# Smoke Test Run Log — 2026-03-20

**Result: PASS (23/23 keys)**
**Duration: 15s | Total updates: 3,171**

---

## 1. Server Boot

```
[2026-03-20T17:06:03.855] INFO  boot wsPort=8080 feedPort=9001
[2026-03-20T17:06:03.856] INFO  listening wsPort=8080 feedPort=9001
```

## 2. Feed Injector (TCP → port 9001)

Synthetic ticks for 5 symbols at 100ms intervals, plus order book add messages (bid + ask per tick).

```
[feed-inject] Connected to localhost:9001
[feed-inject] Sending ticks for: NEXO, VALT, BLITZ, QBIT, FLUX every 100ms
[feed-inject] Sent 245 ticks, 490 OB messages
[feed-inject] Sent 495 ticks, 990 OB messages
[feed-inject] Sent 745 ticks, 1490 OB messages
[feed-inject] Sent 995 ticks, 1990 OB messages
[feed-inject] Sent 1245 ticks, 2490 OB messages
[feed-inject] Disconnected. Total: 1460 ticks, 2920 OB messages
```

## 3. Smoke Client — Subscribe

Client connects, sends a single subscribe message with 23 requests. Server acks all 23.

```
[smoke] Connected to ws://localhost:8080
[smoke] Sent 23 subscribe requests
[smoke]   ack key=1
[smoke]   ack key=2
[smoke]   ack key=3
  ...
[smoke]   ack key=60
```

Server-side subscribe log (all 23 registered in <1ms):

```
[2026-03-20T17:06:23.325] INFO  ws.accepted sessionId=1
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=1 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=2 symbol=NEXO field=volume
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=3 symbol=NEXO field=sma_5
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=4 symbol=NEXO field=sma_20
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=5 symbol=NEXO field=ema_12
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=6 symbol=NEXO field=rsi_14
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=7 symbol=NEXO field=macd_line
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=8 symbol=NEXO field=bollinger_upper
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=9 symbol=VALT field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=10 symbol=BLITZ field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=20 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=21 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=22 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=23 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=30 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=31 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.328] INFO  ws.subscribe key=32 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=40 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=41 symbol=NEXO field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=50 symbol=QBIT field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=51 symbol=FLUX field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=52 symbol=BLITZ field=lastPrice
[2026-03-20T17:06:23.329] INFO  ws.subscribe key=60 symbol=NEXO field=lastPrice
```

## 4. Steady-State Data Flow (sample tick burst)

Each inbound tick fans out to all matching subscriptions. One NEXO tick produces ~20 outbound updates:

```
[smoke]   key=6   NEXO   NEXO/rsi_14                      = 67.57
[smoke]   key=4   NEXO   NEXO/sma_20                      = 132.1095
[smoke]   key=5   NEXO   NEXO/ema_12                      = 132.3210
[smoke]   key=1   NEXO   NEXO/lastPrice                   = 132.7327
[smoke]   key=7   NEXO   NEXO/macd_line                   = 0.2384
[smoke]   key=32  NEXO   NEXO/Atomic(ob.level.bid.1.price) = 132.4374
[smoke]   key=30  NEXO   NEXO/Atomic(rsi_14)              = 67.57
[smoke]   key=23  NEXO   NEXO/Worker(last→scale)          = 13273.27
[smoke]   key=2   NEXO   NEXO/volume                      = 2327
[smoke]   key=22  NEXO   NEXO/Worker(spread)              = 0.3270
[smoke]   key=31  NEXO   NEXO/Atomic(ob.spread)           = -1.8951
[smoke]   key=8   NEXO   NEXO/bollinger_upper             = 132.7069
[smoke]   key=3   NEXO   NEXO/sma_5                       = 132.5073
[smoke]   key=21  NEXO   NEXO/Worker(max)                 = 132.7327
[smoke]   key=20  NEXO   NEXO/Worker(mean)                = 132.5403
```

Multi-symbol keys fire independently per symbol:

```
[smoke]   key=9   VALT   VALT/lastPrice                   = 111.5615
[smoke]   key=10  BLITZ  BLITZ/lastPrice                  = 100.9797
[smoke]   key=52  BLITZ  BLITZ/Worker(max)                = 101.1922
[smoke]   key=50  QBIT   QBIT/Worker(mean)                = 121.5959
[smoke]   key=51  FLUX   FLUX/Atomic(macd_line)           = 0.2274
```

## 5. Timer-Driven Updates (Interval)

key=40 (1000ms Interval → sma_5) and key=41 (500ms Interval → ob.spread) fire at their configured rates:

```
[smoke]   key=41  NEXO   NEXO/Interval(ob.spread)         = -2.0576        (@ 0.5s)
[smoke]   key=40  NEXO   NEXO/Interval(sma_5)             = 132.6888       (@ 1.0s)
[smoke]   key=41  NEXO   NEXO/Interval(ob.spread)         = -2.3792        (@ 1.0s)
  ...
```

Final counts: key=40 got 15 updates (15s / 1s = 15), key=41 got 31 updates (15s / 0.5s ~ 30).

## 6. Aggregate Batching

key=60 (Aggregate arity=5) fires a burst of 5 values every 5 ticks:

```
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.4057
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.4529
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.5700
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.7327
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.7142
  (next burst after 5 more ticks)
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.4772
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.4522
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.6641
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 132.7960
[smoke]   key=60  NEXO   NEXO/Aggregate(5)                = 133.0545
```

Total: 145 updates (149 ticks / 5 * 5 values per batch ~ 145).

## 7. Cancel + Teardown

After duration, client cancels all keys. Server acks all 23:

```
[2026-03-20T17:06:38.323] INFO  ws.cancel key=1
[2026-03-20T17:06:38.323] INFO  ws.cancel key=2
  ...
[2026-03-20T17:06:38.323] INFO  ws.cancel key=60
[2026-03-20T17:06:39.326] WARN  ws.close_failed err=Operation canceled
[2026-03-20T17:06:43.054] INFO  stopped
```

Note: `ws.close_failed err=Operation canceled` is expected — the client closes the socket before the server's async_close completes.

## 8. Final Report

```
  Key    Subscription                         Updates   First (s)
  ---------------------------------------------------------------
  1      NEXO/lastPrice                           149         0.1
  2      NEXO/volume                              149         0.1
  3      NEXO/sma_5                               149         0.1
  4      NEXO/sma_20                              149         0.1
  5      NEXO/ema_12                              149         0.1
  6      NEXO/rsi_14                              149         0.1
  7      NEXO/macd_line                           149         0.1
  8      NEXO/bollinger_upper                     149         0.1
  9      VALT/lastPrice                           149         0.1
  10     BLITZ/lastPrice                          149         0.1
  20     NEXO/Worker(mean)                        149         0.1
  21     NEXO/Worker(max)                         149         0.1
  22     NEXO/Worker(spread)                      149         0.1
  23     NEXO/Worker(last→scale)                  149         0.1
  30     NEXO/Atomic(rsi_14)                      149         0.1
  31     NEXO/Atomic(ob.spread)                   149         0.1
  32     NEXO/Atomic(ob.level.bid.1.price)        149         0.1
  40     NEXO/Interval(sma_5)                      15         1.0
  41     NEXO/Interval(ob.spread)                  31         0.5
  50     QBIT/Worker(mean)                        149         0.1
  51     FLUX/Atomic(macd_line)                   149         0.1
  52     BLITZ/Worker(max)                        149         0.1
  60     NEXO/Aggregate(5)                        145         0.5

  Duration: 15s | Total updates: 3171 | Keys with data: 23/23
  PASS
```

## Bug Found & Fixed During Testing

**Interval node child lifetime bug**: `Interval::child_` was stored as `weak_ptr<INode>`,
but the child (AtomicAccessor) created inside `buildOne` was not added to the `keepAlive`
vector. The child was destroyed immediately after construction, and `child_.lock()` returned
null on the first timer fire.

**Fix**: Changed `Interval::child_` from `weak_ptr` to `shared_ptr` in `include/gma/nodes/Interval.hpp`.
No cycle risk since Interval is a source node with no upstream references.

Files changed:
- `include/gma/nodes/Interval.hpp` — `weak_ptr<INode> child_` → `shared_ptr<INode> child_`
- `src/nodes/Interval.cpp` — removed `.lock()` call in `timerLoop()`
