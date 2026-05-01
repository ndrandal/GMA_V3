# Atomic keys — bare vs `ob.*` (ENC-94)

GMA exposes two distinct atomic-key namespaces. Subscribers must pick
the right one for the data source they're behind, or they'll get
silence.

> **Background:** the 2026-05-01 PoC subscribed to bare `bid` against
> `feed-sim.v3m.xyz` (an ITCH 5.0 feed) and received zero updates,
> while bare `lastPrice` worked fine. This doc explains why and what
> to subscribe to instead.

## The split

| Namespace | Source | When it fires |
|---|---|---|
| **bare** (`lastPrice`, `bid`, `ask`, `spread`, `volume`, ...) | `MarketTickComputer::compute` (in the market connector) | Only when the inbound tick payload carries that JSON field directly. Driven by `MarketFieldMap.bidFields`/`askFields`/etc. — i.e. **pre-aggregated tick connectors**. |
| **`ob.*`** (`ob.best.bid.price`, `ob.spread`, `ob.mid`, ...) | `ob::Provider` via `AtomicProviderRegistry::registerNamespace("ob", …)` | Always available when the market connector is wired and the order book has any state. **Derived from `OrderBookManager`** — the right answer for L2/L3 sources like ITCH/FIX. |

The split is intentional. It preserves the distinction between "the
feed told us this value" and "we computed this from order-book state."
Conflating them would lose information that matters when debugging
data quality.

## Decision rule

| Your feed source emits ... | Subscribe to |
|---|---|
| Pre-aggregated ticks with explicit `bid`/`ask`/`lastPrice` JSON fields | **bare** keys |
| Raw L2/L3 messages (ITCH, FIX, OUCH) | **`ob.*`** keys for top-of-book / depth |
| Both (some feeds carry both NBBO ticks AND raw books) | bare for tick fields, `ob.*` for depth-derived metrics |

Never subscribe to both for the same conceptual value — they may
disagree, and the disagreement is meaningful (your tick was stale or
your book is stale).

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
- `macd.line`, `macd.signal`, `macd.histogram`
- `bb.upper`, `bb.middle`, `bb.lower`, `bb.width`
- `volatility_rank`

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

> **Keep in sync with `ObKeysCatalog.hpp`.** This page is hand-
> transcribed; if you add an `ob.*` key, update both. A regen tool
> would be welcome but isn't shipped.

## Example: PoC against `feed-sim.v3m.xyz`

```ini
# gma.conf — ITCH source, so use ob.* for top-of-book
ingress.0.kind = market.feedserver
ingress.0.port = 9001
ingress.1.kind = market.wsclient
ingress.1.url  = wss://feed-sim.v3m.xyz/feed
ingress.1.adapter = itch
ingress.1.symbols = NEXO,VALT,RAYM,STRT,DRRB
```

```jsonc
// WS subscribe — pick fields that match the source
{
  "type": "subscribe",
  "requests": [
    {"key": 1, "streamKey": "NEXO", "field": "lastPrice"},          // bare — works (trade events)
    {"key": 2, "streamKey": "NEXO", "field": "ob.best.bid.price"},  // ob.*  — works (book derived)
    {"key": 3, "streamKey": "NEXO", "field": "ob.spread"},          // ob.*  — works
    {"key": 4, "streamKey": "NEXO", "field": "bid"}                 // bare — silent (ITCH has no bid field)
  ]
}
```

The first three return value updates; the fourth never fires because
ITCH doesn't carry a JSON `bid` field — the order-book derivation is
the only signal source.
