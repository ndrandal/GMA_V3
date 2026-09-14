# Atomic keys — bare vs `ob.*` (ENC-94, ENC-101)

GMA exposes two distinct atomic-key namespaces. Subscribers must
respect the **push-vs-pull asymmetry** between them, or they'll get
silence.

> **Background:**
> - The 2026-05-01 PoC subscribed to bare `bid` against
>   `feed-sim.v3m.xyz` (an ITCH 5.0 feed) and received zero updates,
>   while bare `lastPrice` worked. ENC-94 documented why.
> - The 2026-05-01 PoC then subscribed to `ob.best.bid.price` /
>   `ob.spread` directly and **also** received zero updates. ENC-101
>   diagnosed the cause: `ob::Provider` writes the AtomicStore on
>   book updates but does not call `Dispatcher::notifyListeners`,
>   so a `Listener` bound to `ob.*` registers and silently never
>   fires.
>
> Phase 1 of the ENC-101 fix (this doc + a construct-time reject
> in `nodes::Listener::Create`) makes the asymmetry explicit
> instead of silent.

## The split

| Namespace | Source | Path |
|---|---|---|
| **bare** (`lastPrice`, `bid`, `ask`, `spread`, `volume`, ...) | `MarketTickComputer::compute` (in the market connector) | **Push.** Fires only when the inbound tick payload carries the JSON field directly (driven by `MarketFieldMap.bidFields`/`askFields`/etc.) — i.e. **pre-aggregated tick connectors**. Reaches subscribers via `Dispatcher::notifyListeners` from `MarketTA.cpp`. |
| **`ob.*`** (`ob.best.bid.price`, `ob.spread`, `ob.mid`, ...) | `ob::Provider` via `AtomicProviderRegistry::registerNamespace("ob", …)` | **Pull-only.** Always present in the AtomicStore when the market connector is wired and the order book has any state — but `ob::Provider` does **not** call `notifyListeners`. Reachable only via `AtomicAccessor` reading the store. |
| **injected raw** (any numeric field name on an inbound tick) | the inbound tick payload itself, written by `Dispatcher::onTick` (ENC-1007) | **Both.** Every numeric field of a tick is written into the AtomicStore under its own name, so it is readable via `AtomicAccessor`; if a Listener is bound to that field it is also pushed as before. |

The split is intentional. It preserves the distinction between "the
feed told us this value" (bare) and "we computed this from
order-book state" (`ob.*`). Conflating them would lose information
that matters when debugging data quality, and pushing every book
update through the dispatcher would amplify message rates beyond
the dispatcher's design budget.

### Injected (externally computed) series — ENC-1007

An "atomic" here is not a concurrency atomic: it is a value derived
from a live feed **outside** gma — a client bringing its own RSI
rather than having gma recompute it. Push a tick for any stream key
carrying any numeric field, and that field lands in the AtomicStore
under its own name.

Two consequences worth knowing:

- **No Listener is required.** An `Interval` + `AtomicAccessor` tree is
  a *pull* consumer that registers with nothing, so the value has to
  materialise in the store whether or not anything is subscribed —
  otherwise §8.1's "on an interval, grab that atomic value" is
  unreachable for injected series.
- **Derived values win a name clash** (default). Builtin atomics
  (`mean`, `sum`, `stddev`, …) live in a flat per-symbol namespace
  keyed by bare function name, so a raw field literally called `mean`
  collides with the builtin `mean`. The raw write happens *first*, so
  the derived value is the last writer and no existing binding changes
  meaning. ENC-1008 namespaces the two apart behind
  `atomicKeyNamespaceByField` — see
  [Derived-atomic key shape](#derived-atomic-key-shape--atomickeynamespacebyfield-enc-1008)
  below.

Admission is bounded by the same `maxSymbols` / `maxFieldsPerSymbol`
budget that bounds per-field history, so injection cannot grow the
store without limit. Once a cap is reached, **new** symbols/fields are
rejected; already-admitted ones keep updating.

## Derived-atomic key shape — `atomicKeyNamespaceByField` (ENC-1008)

`Dispatcher::computeAndStoreAtomics` recomputes the FunctionMap
builtins over **one field's** history and writes the result into the
AtomicStore. Which key it writes is switchable:

| `atomicKeyNamespaceByField` | Store key | Status |
|---|---|---|
| `false` | `<fn>` — bare, e.g. `mean` | **Default.** Pre-ENC-1008 behaviour. |
| `true` | `<field>.<fn>` — e.g. `alpha.mean` | Opt-in. |

### The defect the flag fixes

The bare key is **one slot per symbol per builtin**, shared by every
field of that symbol. With a symbol whose `alpha` and `beta` fields
both drive `mean`, the second field to tick overwrites the first and
that value is gone:

```
alpha history [10, 20]   -> mean  15   ─┐
                                        ├─ both write (SYM, "mean")
beta  history [100, 200] -> mean 150   ─┘   -> store holds 150; 15 is lost
```

Turn the flag on and they become `alpha.mean = 15` and
`beta.mean = 150`, which is the whole point of the ticket.

The single-primary-field assumption the old shape encoded was never an
external requirement — `docs/CODE_REVIEW_2026-06-26.md` §M9 records it
as a **CONFIRMED** defect whose two sanctioned fixes were "incorporate
`field` into the key, **or** document/enforce the single-field
assumption", and ENC-792 took the second branch. The flag exists
because the key is client-visible, not because flat is correct.

### Why it is a flag, and why it defaults off

The store key is a **client-supplied wire string**. `field` in a WS
subscribe request is read verbatim by `TreeBuilder` and used as both
the `Listener` subscription key (push) and the `AtomicAccessor` field
(pull). There is no version negotiation on it, so the shape cannot
simply change under existing clients.

### Exactly what changes when you flip it on

- **AtomicStore keys for derived builtins move** from `<fn>` to
  `<field>.<fn>`. An **`AtomicAccessor` bound to a bare builtin name
  stops resolving.** This is the breaking half.
- **Listener push does not change.** A subscriber on the bare `mean`
  still receives every field's mean, exactly as before;
  `<field>.<fn>` becomes *additionally* subscribable and narrows to
  one field. The outbound WS surface carries no key string at all
  (`update` frames are `type` / request key / `streamKey` / `value`),
  so nothing on the response side needs migrating.
- **Only the Dispatcher's builtins move.** `MarketTickComputer`'s bare
  keys (`lastPrice`, `bid`, `sma_5`, its own `mean`/`median`, …),
  `ob.*` and the ENC-1007 raw injected fields are untouched in both
  states. One side effect worth knowing: with the flag on the
  Dispatcher vacates the bare `mean`/`median`/`spread` keys, so
  MarketTA's same-named values stop being contested in the store.
  MarketTA's own notify-suppression list (`_skipFields`) is unchanged.
- **Key cardinality grows.** Derived keys multiply by the number of
  listened fields, so `maxFieldsPerSymbol` / `AtomicStore::setCaps`
  budgets are consumed faster.
- **Dotted keys meet the provider grammar.** `AtomicProviderRegistry`
  splits a key on its first `.` and treats the prefix as a provider
  namespace. A *present* `alpha.mean` is served from the store and
  never reaches that path, but a *missing* one will probe for a
  provider named `alpha`. Harmless today (only `ob` and `synthetic`
  are registered) — but do not register a provider namespace that
  collides with a field name.
- **A field named after a provider namespace can collide.** The new key
  is a plain `field + "." + fn` concatenation, so a tick field
  literally called `ob` driving the builtin `spread` writes
  `ob.spread` — the same key `ob::Provider` owns. Nothing rejects it:
  `nodes::Listener::Create` only refuses fields with a literal `ob.`
  prefix, and bare `ob` is two characters. The builtin names that make
  this reachable are `spread`, `mid`-adjacent reducers and anything
  else in `ObKeysCatalog`. Narrow and opt-in-only, but it is a genuine
  new silent-overwrite path — do not name a tick field `ob` with this
  flag on.

### Migration checklist before flipping it on

Anything that names a bare builtin as a `field` must move to
`<field>.<fn>`:

- checked-in WS corpora — `tools/nl-tree/corpus_ws_messages.json`,
  `tools/nl-tree/corpus.json`, `tests/treebuilder/corpus_requests.json`
  (`"field": "mean"` / `"median"` inside `AtomicAccessor` nodes);
- `tools/nl-tree/schema.py`'s "Available atomic fields" list, which is
  what the NL tool generates client trees from;
- **saved pipelines in the forum DB**, which are stored client JSON and
  cannot be migrated from this repo.

Note that the corpora's `mean`/`median` are `MarketTickComputer` keys,
which do **not** move — check each one against its actual producer
rather than renaming on sight.

## Push vs pull — the rule

| You want to ... | Do this |
|---|---|
| Subscribe directly to a bare key (Listener-on-`field`) | ✅ `{ "streamKey": "...", "field": "lastPrice" }` |
| Subscribe directly to an `ob.*` key (Listener-on-`field`) | ❌ **Rejected at construct time.** The reject lands as a `{"type":"error","where":"build","message":"listener: field '...' is pipeline-only — see docs/atomic-keys.md..."}` WS frame. |
| Surface an `ob.*` value into a chart / responder | ✅ Use the **canonical pattern** — a Listener on a bare key as a clock + `AtomicAccessor` reading the `ob.*` value from the store. See below. |

A `Listener` bound to an `ob.*` field would silently fail today's
ENC-101 repro, so the construct-time reject (in
`nodes::Listener::Create`, since 2026-05-06) is strictly louder than
the prior silent failure.

## Canonical `ob.*`-in-a-chart pattern

The Listener acts as a **clock**: every bare-key tick triggers
downstream nodes. The `AtomicAccessor` ignores the trigger's value
and reads the named `ob.*` key from the `AtomicStore`. The result
flows to the responder.

```
Listener(NEXO.lastPrice)  →  AtomicAccessor(NEXO.ob.best.bid.price)  →  Responder
                ▲                              ▲
                │                              │
        fires on each NEXO trade        on each fire, reads
        event from MarketTA             the latest ob.best.bid.price
                                        from AtomicStore
```

The cadence of updates equals the cadence of the chosen clock.
`lastPrice` (a bare key, push-fed by the trade-event path) is the
default clock for "I want updates as fast as trades happen". For a
slower clock, swap in an `Interval` source (e.g. every 250ms) — see
`buildSimple()` in `src/core/TreeBuilder.cpp` for the
Interval-driven shape.

### Worked WS subscribe — NEXO best-bid price

```jsonc
{
  "type": "subscribe",
  "requests": [
    {
      "key": 1,
      "streamKey": "NEXO",
      "field": "lastPrice",
      "node": {
        "type": "AtomicAccessor",
        "streamKey": "NEXO",
        "field": "ob.best.bid.price"
      }
    }
  ]
}
```

The Listener registers on `(NEXO, lastPrice)`. On each trade event
the AtomicAccessor reads the current `(NEXO, ob.best.bid.price)`
from the store and forwards a `StreamValue` containing the
top-of-book bid. The WS responder emits an `update` frame:

```jsonc
{ "type": "update", "key": 1, "streamKey": "NEXO", "value": 24.83 }
```

### Worked WS subscribe — NEXO bid-ask spread (composed)

For derived values, chain a `Worker` after a multi-input
aggregation. The TreeBuilder's `pipeline` array form expresses this:

```jsonc
{
  "type": "subscribe",
  "requests": [
    {
      "key": 1,
      "streamKey": "NEXO",
      "field": "lastPrice",
      "pipeline": [
        { "type": "AtomicAccessor", "streamKey": "NEXO", "field": "ob.best.bid.price" },
        { "type": "Worker", "fn": "diff" }
      ]
    }
  ]
}
```

(Production charts that need both bid and ask will use a
two-AtomicAccessor + Aggregate(arity=2) + Worker(spread) shape;
see `forum/db/seed/seed.go`'s `NEXO bid-ask spread` pipeline.)

### Subscribe request key — int vs string (`RequestKey`)

Each subscribe request must carry a per-request identifier. The
wire accepts three input forms — `key:<int>`, `id:<int>` (legacy
fallback), or `id:"<string>"`. Outbound `subscribed` ack frames and
`update` frames mirror the input type via two distinct field
names: `key:<int>` for int-keyed subs, `requestId:"<string>"` for
string-keyed subs. Supplying both `key` and `id` on the same
request is a protocol error, as is mixing `keys:[int,...]` and
`ids:[string,...]` on a single `cancel` payload.

Internal routing — Dispatcher, AtomicStore, TreeBuilder, Listener —
stays key-type-agnostic: subscribers route on `(streamKey, field)`,
not on the request id. The variant lives at the wire boundary
(`gma::server::RequestKey` in `include/gma/server/RequestKey.hpp`).

```jsonc
// int-keyed subscribe (smoke.js, int-native consumers)
{ "type": "subscribe", "requests": [ { "key": 1, "streamKey": "NEXO", "field": "lastPrice" } ] }
// → { "type": "update", "key": 1, "streamKey": "NEXO", "value": 24.83 }

// string-id subscribe (embassy, saved-scene consumers)
{ "type": "subscribe", "requests": [ { "id": "r-NEXO-open", "streamKey": "NEXO", "field": "lastPrice" } ] }
// → { "type": "update", "requestId": "r-NEXO-open", "streamKey": "NEXO", "value": 24.83 }
```

## Decision rule

| Your feed source emits ... | Subscribe via |
|---|---|
| Pre-aggregated ticks with explicit `bid`/`ask`/`lastPrice` JSON fields | **Listener** on the **bare** field |
| Raw L2/L3 messages (ITCH, FIX, OUCH) | **Listener** on a bare clock (`lastPrice` is the default) + an `AtomicAccessor` pipeline node naming the `ob.*` field |
| Both | bare directly for the tick fields, canonical pattern for `ob.*` depth-derived metrics |

Never name the same conceptual value via both paths in one
subscription — they may disagree, and the disagreement is meaningful
(your tick was stale or your book is stale).

## Bare-key vocabulary

Tick fields written by `MarketTickComputer` when the tick payload
contains them under a name listed in `MarketFieldMap`:

- `lastPrice`, `openPrice`, `highPrice`, `lowPrice`, `prevClose`
- `bid`, `ask`, `spread`
- `volume`, `obv`, `vwap`
- `mean`, `median`
- `timestamp`

TA indicators computed by `MarketTickComputer` from price/volume
history (parameters from `Config.ta*`):

- `sma_<n>` (e.g. `sma_5`, `sma_20`)
- `ema_<n>` (e.g. `ema_12`, `ema_26`)
- `rsi_<n>` (e.g. `rsi_14`)
- `atr_<n>`, `momentum_<n>`, `roc_<n>`, `volume_avg_<n>`
- `macd_line`, `macd_signal`, `macd_histogram` (underscored — the engine
  emits no dotted `macd.*` keys)
- `bollinger_upper`, `bollinger_lower` (only these two — the engine emits
  no `bollinger_middle`/`bollinger_width` and no `bb.*` keys)
- `volatility_rank`

All bare keys are **Listener-subscribable**.

## `ob.*` vocabulary

Top-of-book and book-derived metrics from `ob::Provider`. Canonical
list (transcribed from
`connectors/market/include/gma/ob/ObKeysCatalog.hpp`):

- `ob.best.bid.price`, `ob.best.bid.size`
- `ob.best.ask.price`, `ob.best.ask.size`
- `ob.spread`, `ob.mid`
- `ob.cum.bid.levels.10.size`
- `ob.cum.ask.levels.10.size`
- `ob.imbalance.levels.1-10`
- `ob.vwap.bid.levels.1-10`
- `ob.vwap.ask.levels.1-10`
- `ob.meta.seq`, `ob.meta.is_stale`

All `ob.*` keys are **pipeline-only** (never Listener-subscribable).
The construct-time reject in `nodes::Listener::Create` rejects any
field whose first three characters are `o`, `b`, `.`.

> **Keep in sync with `ObKeysCatalog.hpp`.** This page is hand-
> transcribed; if you add an `ob.*` key, update both. A regen tool
> would be welcome but isn't shipped.

## Example: PoC against `feed-sim.v3m.xyz`

```ini
# gma.conf — ITCH source, so use the canonical pattern for ob.*
ingress.0.kind = market.feedserver
ingress.0.port = 9001
ingress.1.kind = market.wsclient
ingress.1.url  = wss://feed-sim.v3m.xyz/feed
ingress.1.adapter = itch
ingress.1.symbols = NEXO,VALT,RAYM,STRT,DRRB
```

```jsonc
// WS subscribe — pick the right path per field
{
  "type": "subscribe",
  "requests": [
    // bare keys: subscribe directly
    {"key": 1, "streamKey": "NEXO", "field": "lastPrice"},
    {"key": 2, "streamKey": "NEXO", "field": "sma_5"},

    // ob.* keys: canonical pattern (clock + AtomicAccessor)
    {
      "key": 3,
      "streamKey": "NEXO",
      "field": "lastPrice",
      "node": {
        "type": "AtomicAccessor",
        "streamKey": "NEXO",
        "field": "ob.best.bid.price"
      }
    },

    // The pre-ENC-101 anti-pattern — DON'T do this. It now fails
    // at construct time with a `where=build, pipeline-only` error.
    // {"key": 4, "streamKey": "NEXO", "field": "ob.best.bid.price"}
  ]
}
```

Keys 1, 2, 3 all return value updates. The commented-out anti-pattern
would have silently registered a Listener that never fires; since
the ENC-101 fix it returns an immediate error frame and is never
created.

## Reproducing ENC-101's regression

ENC-101's original PoC ran for 15 seconds against
`feed-sim.v3m.xyz`'s ITCH stream subscribing to
`ob.best.bid.price` / `ob.best.ask.price` / `ob.spread` for
NEXO+VALT and observed **0 updates**, while bare `lastPrice` and
`sma_5` on the same connection received 170 events (all from the
trade-event push path in `MarketTA.cpp:400`). The cause was the
silent push-vs-pull asymmetry this doc now spells out.

The empirical proof of the fix is in
`tests/integration/Enc101CanonicalPatternTest.cpp`. It drives the
full canonical-pattern topology
(`Listener("NEXO","lastPrice") → AtomicAccessor("NEXO","ob.best.bid.price") → terminal`)
using the same primitives the production push path uses —
`Dispatcher::notifyListeners` for the trade-event push,
`AtomicStore::set` for what `ob::Provider` writes on book updates —
so the proof runs deterministically without needing live feed-sim.

Run:

```sh
cd GMA_V3
mkdir -p build && cd build
cmake .. -DGMA_BUILD_TESTS=ON
cmake --build . -j --target gma_tests
./gma_tests --gtest_filter='Enc101CanonicalPatternTest.*'
```

Expected:

```
[ RUN      ] Enc101CanonicalPatternTest.ListenerClockPlusAtomicAccessorEmitsObValues
[       OK ] (3 terminal hits — ENC-101 saw 0 here)
[ RUN      ] Enc101CanonicalPatternTest.ListenerOnObFailsAtConstructTime
[       OK ] (the old anti-pattern is rejected with `pipeline-only`)
```

The `ListenerClockPlusAtomicAccessorEmitsObValues` case fires three
synthetic trade events through `Dispatcher::notifyListeners` and
asserts the terminal received **3** values, all carrying a real
`ob.best.bid.price` from the store. ENC-101's original baseline at
the same scope was **0**.

### Live feed-sim run (optional)

When feed-sim is reachable, the same pattern works end-to-end on a
real WebSocket connection. Stand up `gma_server` configured for the
ITCH ingress (see `gma.conf` example near the top of this doc),
then a small WS client posts the canonical request:

```python
import asyncio, json, websockets

REQUEST = {
    "type": "subscribe",
    "requests": [{
        "key": 1,
        "streamKey": "NEXO",
        "field": "lastPrice",
        "node": {
            "type": "AtomicAccessor",
            "streamKey": "NEXO",
            "field": "ob.best.bid.price",
        },
    }],
}

async def main(duration_s: float = 15.0) -> int:
    n = 0
    async with websockets.connect("ws://localhost:4000") as ws:
        await ws.send(json.dumps(REQUEST))
        try:
            async with asyncio.timeout(duration_s):
                async for msg in ws:
                    frame = json.loads(msg)
                    if frame.get("type") == "update":
                        n += 1
        except asyncio.TimeoutError:
            pass
    return n

if __name__ == "__main__":
    count = asyncio.run(main())
    print(f"{count} update(s) in 15s")
    assert count >= 1, "ENC-101 baseline was 0; canonical pattern must produce ≥1"
```

ENC-101's pre-fix repro: 0 updates in 15s. Post-fix: should track
the trade-event cadence on NEXO (typically dozens to hundreds in
that window).
