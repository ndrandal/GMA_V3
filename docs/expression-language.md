# Expression Language (request-tree compute model)

GMA_V3 request trees compose a small set of dataflow **nodes** over streaming
values. This doc covers the *expression-language* layer (project
`2026-06-17-gma-expression-language`) that turns the fixed reducer menu into a
composable language: structured values, user-defined formulas, conditionals,
and reusable bindings.

**Design invariant — bounded cost per tick.** Every node added here is *total*:
each subscription's per-event cost is statically boundable. There are no loops,
no recursion, and no data-dependent unbounded work. Expression size is bounded
by the validator's depth cap (`MAX_TREE_DEPTH = 32`).

---

## Values: scalars and records

A pipeline edge carries an `ArgType` — a scalar (`bool`/`int`/`double`/string),
a vector, or a **`Record`**: an insertion-ordered set of named fields
(`include/gma/StreamValue.hpp`). A record lets one pipeline carry e.g. an OHLC
candle instead of N parallel scalar pipelines. Records serialize to JSON objects
(`include/gma/util/JsonUtil.hpp`).

### `Pack` — assemble a record (fan-in, combineLatest)

```json
{"type":"Pack","fields":{
  "bid":{"type":"Listener","streamKey":"SYM","field":"bid"},
  "ask":{"type":"Listener","streamKey":"SYM","field":"ask"}
}}
```

Each field has its own input subtree. Once **every** field has produced a value
for a symbol, each subsequent field update emits a `Record` of the latest
values, in declared order.

### `Field` — extract one field

```json
{"type":"Field","name":"bid"}
```

Forwards the named field's value from an incoming record. Non-records and absent
fields are dropped.

---

## `Expr` — user-defined expressions

Evaluates a compiled expression over each incoming value and forwards the scalar
result. A **record** input exposes each field as a named ref; a **scalar** input
is exposed as the ref `"value"`.

```json
{"type":"Expr","expr":{"op":"div","args":[
  {"op":"add","args":[{"ref":"bid"},{"ref":"ask"}]}, 2]}}
```

### Grammar

| Form | Meaning |
|------|---------|
| `3.5`, `true` / `{"lit": <num\|bool>}` | literal |
| `{"ref":"name"}` | named input (0.0 if absent at eval) |
| `{"op":"<operator>","args":[ … ]}` | operator application |
| `{"op":"fn","name":"<builtin>","args":[ … ]}` | call a `FunctionMap` builtin |

Operators (numeric; comparisons/logical yield `1.0`/`0.0`, truthy = `> 0.5`):

- **n-ary:** `add` `mul` `min` `max` `and` `or`
- **binary:** `sub` `div` `mod` `gt` `lt` `gte` `lte` `eq` `neq`
- **unary:** `neg` `abs` `not`

`div`/`mod` by zero yield `0.0` (finite/total — no NaN/inf). The `fn` bridge
reaches all ~55 `FunctionMap` builtins (`sum`, `mean`, `sqrt`, …). Malformed
expressions (unknown op/fn, wrong arity) throw at **build time**, never at eval.

### `select` / `iff` — value-level branch

`select(cond, a, b) → a if cond>0.5 else b` is a `FunctionMap` builtin, most
useful as a ternary inside an expression:

```json
{"op":"fn","name":"select","args":[ {"op":"gt","args":[{"ref":"rsi"},70]}, 1, 0 ]}
```

---

## Control flow

### `Filter` — predicate gate

Forwards each value **unchanged** iff a predicate expression over it is truthy.

```json
{"type":"Filter","when":{"op":"gt","args":[{"ref":"rsi"},70]}}
```

### `Switch` — value-conditioned routing

Routes each value (unchanged) to one of N case branches by a selector expression
(rounded to an index); out-of-range goes to the optional `default`, else drops.

```json
{"type":"Switch","select":{"ref":"regime"},
 "cases":[<bear>, <flat>, <bull>], "default":<other>}
```

---

## Reuse: `Tee`, `Let`, `Ref`

### `Tee` — broadcast fan-out

Forwards each value to **every** output subtree (compute the upstream once, run
N branches):

```json
{"type":"Tee","outputs":[<branchA>, <branchB>]}
```

### `Let` / `Ref` — named bindings (reuse DAG)

A `Let` names producer subtrees that `Ref` taps across its body. Each referenced
binding's producer is built **once** and fanned (via `Tee`) to all its
consumers — so an intermediate is computed once, not recomputed per use.

```json
{"type":"Let",
 "bindings":{ "mid":{"type":"AtomicAccessor","field":"mid"} },
 "body":{"type":"Aggregate","arity":2,"inputs":[
    {"type":"Chain","stages":[{"type":"Ref","name":"mid"}, <exprA>]},
    {"type":"Chain","stages":[{"type":"Ref","name":"mid"}, <exprB>]}
 ]}}
```

Sibling bindings cannot reference each other (no cycles). Bindings are resolved
at build time, so `Ref` is unsupported inside lazily-built subtrees (e.g.
`GroupSplit` children).

---

## Worked example — "emit price when RSI > 70"

```
Pack{rsi, price}  ->  Filter{ when: rsi > 70 }  ->  Field{ price }
```

```json
[
  {"type":"Pack","fields":{
    "rsi":{"type":"Listener","streamKey":"SYM","field":"rsi"},
    "price":{"type":"Listener","streamKey":"SYM","field":"price"}
  }},
  {"type":"Filter","when":{"op":"gt","args":[{"ref":"rsi"},70]}},
  {"type":"Field","name":"price"}
]
```

The record carries both `rsi` and `price`; the filter gates on `rsi`; the field
emits `price` only on the ticks that pass. See
`tests/integration/ExpressionLanguageCapstoneTest.cpp` for the end-to-end test.
