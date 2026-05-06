# SPEC: enshrine `ob.*` as pipeline-only (not Listener-subscribable)

**Slug:** ob-keys-pipeline-only
**Date:** 2026-05-06
**Status:** Draft
**Author:** ndrandal
**Repos:** `GMA_V3` (docs + nodes/Listener), `customer-layer` (apps/web TS types), `forum` (db/seed reshape)
**Linear:** ENC-101

## Problem

A `Listener` node bound to an `ob.*` atomic key (`ob.best.bid.price`,
`ob.spread`, etc.) registers with the Dispatcher but receives **zero
updates**. The PoC validating ENC-94/ENC-99 confirmed this against
feed-sim: 15s of subscription to `ob.best.bid.price` /
`ob.best.ask.price` / `ob.spread` for NEXO + VALT yielded 0 events,
while bare keys (`lastPrice`, `sma_5`) on the same connection fired
normally (170 events). The cause is architectural: only
`connectors/market/src/MarketTA.cpp:400` calls
`Dispatcher::notifyListeners` — `connectors/market/src/ob/ObProvider.cpp`
writes the AtomicStore on book updates but never notifies. The
asymmetry is real and intentional (the OB namespace exists to
express continuously-evolving derived state, not feed events) but
nothing in the system tells a user, an AI-generated pipeline, or
ENC-94's own `docs/atomic-keys.md` that the Listener path is
unavailable for `ob.*`. The result is silent breakage. The seed
pipeline added in mvp-deploy-prep Phase 2 (`forum/db/seed/seed.go:293`,
NEXO bid Listener) is itself an instance of the bug.

## Proposed change

Enshrine `ob.*` as **pipeline-only**: the canonical way to surface
top-of-book values in a chart is `Listener(<bare-key clock>) →
AtomicAccessor(ob.*)` inside a TreeBuilder spec. Make the
asymmetry explicit at every layer that can produce a broken pipeline:

- Documentation that names the rule and shows the canonical pattern.
- A construction-time reject in `gma::nodes::Listener` for any field
  in the `ob.*` namespace, with a clear error message that points
  the developer at the AtomicAccessor pattern.
- A TypeScript-side type narrowing on
  `customer-layer/apps/web/src/types/pipeline-graph.ts` so a
  Listener's `field` cannot be an `ob.*` key at compile time.
- Reshape the seeded NEXO bid pipeline to use the canonical pattern.

This is the lowest-blast-radius option (compared to making
`ObProvider` push events on every book update, which would amplify
message rates and blur the architectural split between feed-event
push and order-book-derived state).

## Scope

- **`GMA_V3/docs/atomic-keys.md`** — replace ENC-94's
  "subscribe to these for L2 sources" framing for `ob.*` with the
  push-vs-pull asymmetry: bare keys are push-fed via MarketTA,
  `ob.*` is pull-only via AtomicStore. Document the canonical
  Listener-clock + AtomicAccessor pattern with a worked example
  (the same NEXO/VALT case the PoC ran).
- **`GMA_V3/include/gma/nodes/Listener.hpp`** + **`src/nodes/Listener.cpp`** —
  `Listener` constructor (or `start()`) returns / throws / refuses
  registration when `field_` matches the `ob.*` namespace. Surface
  via a `Result`-shaped factory (matches the rest of the codebase's
  `Result.hpp` pattern; see `include/gma/Result.hpp`). The error
  message names the field, points at `docs/atomic-keys.md`, and
  suggests the AtomicAccessor alternative.
- **`GMA_V3/src/core/TreeBuilder.cpp`** — wire the new factory so
  a graph that names `ob.*` on a Listener fails to build with the
  same error, before `start()`.
- **`customer-layer/apps/web/src/types/pipeline-graph.ts`** —
  narrow `ListenerNode.config.field` to a non-`ob.*` subset of
  `AtomicKey`. Add a runtime type guard helper
  `isListenerSubscribable(field: AtomicKey): boolean` for callers
  that build pipelines from runtime data (e.g., AI-generated graphs).
- **`forum/db/seed/seed.go`** — reshape the NEXO bid pipeline
  (currently `Listener(ob.best.bid.price)` → AtomicAccessor →
  Responder) to the canonical pattern: `Listener(lastPrice on NEXO)
  → AtomicAccessor(ob.best.bid.price) → Worker(spread fn) →
  Responder`. Update the unit test counts.

## Non-goals

- **No C++ change to `ObProvider` to push events.** The push path
  intentionally carries feed events, not derived order-book state;
  pushing every book update would amplify message rates beyond the
  Dispatcher's design budget and require throttling logic that
  doesn't exist today.
- **No new protocol shape for Interval-based polling on `ob.*`.**
  The canonical pattern (Listener on a high-frequency bare key
  feeding AtomicAccessor) gives the same effective semantics
  without expanding the cloudchannel protocol.
- **No changes to forum's classifier / orchestrator prompt.**
  Post-Phase-6-cutover the classifier is rule-based
  (`forum/internal/orchestrator/classifier.go`), and aibox-mock
  returns canned plans; there is no live LLM prompt today that
  could mis-generate a Listener-on-`ob.*`. When real AI boxes
  replace aibox-mock, the prompt update lands as part of that
  migration, informed by the doc this proposal produces.
- **No GMA_V3 perf changes.** Hot-path benchmarks
  (`PackAppend` 0.85 ns/op / 0 allocs;
  `OrchestratorThroughput` ≥40M ops/sec) must remain unchanged.
- **No backward-compat shim.** No production deployment has a
  Listener on `ob.*` that fires today (it never did); making the
  failure explicit at construct time doesn't break any working
  user pipeline.

## Acceptance criteria

1. **Docs**: `gma_v3/docs/atomic-keys.md` contains a section titled
   "push vs pull" that names the asymmetry, lists the canonical
   pattern, and includes the NEXO/VALT worked example. The misleading
   "subscribe to these for L2 sources" wording is gone.
2. **Listener construct rejects ob.\***: a unit test
   (`src/nodes/Listener_test.cpp` or wherever the existing
   pattern places it) builds a Listener with `field="ob.spread"`
   and asserts the factory returns an error containing the string
   `pipeline-only` and the field name.
3. **TreeBuilder rejects ob.\* listener** at graph-build time
   when a JSON spec names `kind=listener` with `field=ob.best.bid.price`;
   error surfaces in `mage Test` (gma_v3 ws tests) without the
   graph reaching `start()`.
4. **TypeScript compile-time guard**: `pnpm --filter
   @customer-layer/web build` fails if a developer assigns
   `{ kind: "listener", config: { field: "ob.best.bid.price", ... } }`
   to a `ListenerNode`. (i.e., `ListenerNode["config"]["field"]`
   excludes `ob.*` template-literal types).
5. **Seed reshape**: `forum/db/seed/seed.go`'s NEXO bid pipeline
   no longer references `ob.*` as a Listener field.
   `TestRunIsIdempotent` and `TestSeededAdminCanLoginViaSymmetricSecret`
   continue to pass; new pipeline graph nodes parse cleanly through
   the existing JSON marshalling.
6. **End-to-end PoC re-run**: against feed-sim, a TreeBuilder spec
   with the canonical pattern (`Listener(NEXO.lastPrice) →
   AtomicAccessor(NEXO.ob.best.bid.price) → Responder`) emits
   non-zero updates within 15s — the same window where ENC-101's
   original repro saw zero. Documented in
   `gma_v3/docs/atomic-keys.md` as a self-contained `apps/poc-client`
   command.
7. **No bench regression**: `mage Bench` on embassy and
   `mage Bench` on gma_v3 (if present) report unchanged numbers
   for `PackAppend` / hot-path benchmarks (within ±2%).

## Constraints

- **Performance:** No changes to gma_v3 hot path; ObProvider stays
  pull-only. Listener construct gains one string-prefix check
  (sub-microsecond).
- **Compatibility:** Listener-on-`ob.*` was always silently broken;
  making it an explicit construct-time error is strictly an
  improvement, not a wire-format or API break. No proto / treaty
  changes.
- **Dependencies:** None. Self-contained within GMA_V3 +
  customer-layer types + forum seed.
- **Deadline:** None declared. Useful as a follow-up to
  `mvp-deploy-prep` (closes the open assumption in ENC-94's
  `atomic-keys.md`); no calendar pressure.

## Affected systems / callers

- **`GMA_V3/include/gma/nodes/Listener.hpp` +
  `src/nodes/Listener.cpp`** — factory or `start()` gains the
  reject path.
- **`GMA_V3/src/core/TreeBuilder.cpp`** — surfaces the reject
  during graph build.
- **`GMA_V3/docs/atomic-keys.md`** — sharpens ENC-94's doc.
- **`GMA_V3/CLAUDE.md`** — one-line cross-reference under "Code
  Conventions" (the document already lists ENC-94's pointer; this
  proposal extends the wording).
- **`customer-layer/apps/web/src/types/pipeline-graph.ts`** —
  narrows `ListenerNode.config.field` and exports
  `isListenerSubscribable`.
- **`forum/db/seed/seed.go`** — reshapes the NEXO bid pipeline.
- **`forum/db/seed/seed_test.go`** — `TestRunIsIdempotent` count
  assertions stay; pipeline structure assertions update to
  reflect the new node count (Listener + AtomicAccessor + Worker
  + Responder = 4 nodes vs current 2).

No `treaty/` changes. No `embassy/` changes (data plane / GMA
client are unaffected).

## Alternatives considered

- **(a) ObProvider emits `Dispatcher::notifyListeners` on book
  updates.** Rejected: order-book updates run at much higher
  cadence than trade events; pushing them would either flood the
  dispatcher (no upstream throttle) or require a new throttle layer
  that doesn't exist today. The architectural split between feed
  events (push) and derived state (pull-only) is intentional and
  documented in ENC-94; option (a) erodes that without a clear
  user-visible win, since the canonical Listener+AtomicAccessor
  pattern gives the same chart-paint behavior at the
  user's chosen clock cadence.
- **(c) New cloudchannel-protocol Interval-based listener for
  `ob.*`.** Rejected: it's strictly a subset of what the existing
  `Interval → AtomicAccessor` pipeline composition already
  expresses. Adding it as a first-class wire shape costs treaty
  + forum + embassy + gma_v3 touchpoints to reproduce something
  callers can build today.
- **Status-quo + AI-prompt-only fix.** Rejected: makes the
  failure mode "AI-generated pipelines work, hand-rolled graphs
  silently break". The construct-time reject in `Listener`
  closes the silent path uniformly.

## Risks

- **TreeBuilder error path is poorly surfaced today.** If
  TreeBuilder swallows the new `Result` instead of propagating it
  to the WS client, the user gets a `502` with no detail.
  Mitigation: AC #3 gates the spec on `mage Test` exercising the
  WS error path; if the existing `ClientSession.cpp` catch-all
  drops it, this proposal includes the small surfacing change to
  bubble the message up via `sendError("subscribe", msg)` (the
  same shape as
  `src/server/ClientSession.cpp:355`).
- **TypeScript template-literal exclusion subtly fails for
  `(string & {})`.** Existing `AtomicKey` includes
  `(string & {})` as an escape hatch — a `ListenerNode.config.field`
  typed as the same will accept any string at compile time,
  defeating AC #4. Mitigation: define the listener-subscribable
  type as the explicit non-`ob.*` literal union (drops the
  `(string & {})` escape), forcing call sites that genuinely need
  a runtime-only key to call `isListenerSubscribable` first.
- **Seed reshape changes counts.** `TestRunIsIdempotent` checks
  per-table counts; reshape adds two nodes to the NEXO pipeline.
  Mitigation: the test counts row-counts, not pipeline-node-counts
  (nodes are stored as a JSON column, not a separate table); no
  test should break. Verify in AC #5.

## Open questions

- None at spec time.
