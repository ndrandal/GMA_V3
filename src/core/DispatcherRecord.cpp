// Dispatcher::onRecord — the non-JSON ingress path (ENC-1486).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1, D3, D9, D13;
//         docs/data-layer.md §C2, §C6, §C7, §6.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT, AND NOT THREE MORE FUNCTIONS IN
// src/core/Dispatcher.cpp
// ------------------------------------------------------------------------
// Because the one requirement here that cannot be met by intention is "no
// JSON DOM on this path", and a separate file makes it checkable. Every
// token this file contains is the record path; nothing else is in scope. The
// check is `RecordPathNamesNoJson` in
// tests/dispatch/RecordIngressParityTest.cpp, which reads this file and
// `src/core/Dispatcher.cpp` off disk and asserts:
//
//   * this file names none of the ~21 JSON-shaped identifiers in
//     `jsonTokens()` — `rapidjson`, `Document`, `payload`, `Event`, the
//     `*Member` family, the `Get*`/`Is*` accessors, `JsonFieldSource`;
//   * `Dispatcher::ingest` — the body both entry points share — names none of
//     them either;
//   * and, as its POSITIVE CONTROL, that `Dispatcher::onTick` DOES. Same
//     file-reader, same token list, same parser: one function red and one
//     green, so a green verdict cannot be a verdict about a search that
//     looked at nothing. This project has shipped gates that passed because
//     they could not see the thing they were checking; a negative with no
//     true positive beside it is one of them.
//
// This file cannot be *physically* unable to see rapidjson — `Dispatcher.hpp`
// includes `Event.hpp`, which includes `rapidjson/document.h`, and `onTick`
// is a member of the same class. The check is therefore over what the record
// path NAMES, which is the regression that actually threatens it: someone
// "unifying" the paths by synthesizing an `Event` inside `onRecord`.

#include "gma/Dispatcher.hpp"

#include "gma/data/Record.hpp"
#include "gma/data/SymbolTable.hpp"
#include "gma/engine/RecordFieldSource.hpp"
#include "gma/util/Logger.hpp"

#include <atomic>
#include <string>

namespace gma {

void Dispatcher::setSymbolTable(const data::SymbolTable* symbols) {
  _symbols.store(symbols, std::memory_order_release);
}

const std::string& Dispatcher::recordEventType() {
  // "tick", spelled once. A TickRecord is the resolved form of a tick, so it
  // must reach exactly the computers `Event{sym, payload}` reaches:
  // `Event::type` defaults to "tick" and MarketConnector registers its
  // computer factory under "tick". A function-local static rather than a
  // temporary per record, because `ingest` takes a `const std::string&` and
  // the typed-computer cache is keyed by `std::string`.
  static const std::string kTick = "tick";
  return kTick;
}

void Dispatcher::onRecord(const data::TickRecord& rec) {
  const data::SymbolTable* symbols = _symbols.load(std::memory_order_acquire);
  if (!symbols) {
    // No resolver installed. REFUSE rather than degrade: a record names a
    // `symbolId` and nothing else, so without a table there is no stream key.
    // Routing under "" would put every symbol's values into one bucket and
    // every subscription would miss — a silent wrong answer, which is worse
    // than a loud nothing.
    //
    // LOGGED ONCE PER DISPATCHER, AND THE REASON IS BENCHMARK INTEGRITY
    // RATHER THAN LOG TIDINESS. An earlier revision of this block logged
    // unconditionally and claimed in a comment that "the only way to reach it
    // is a wiring mistake that the first record reports" — which was simply
    // false: this branch is reached per RECORD, `util::Logger` has no
    // throttling API at all (see Logger.hpp), and nothing anywhere rate-limits
    // it. `onRecord` is step 2 of ENC-1487's five-step B2 measurement, the
    // number SPEC specs/2026-10-14-gma-data-layer/SPEC.md D13 sizes this whole
    // project by, and it carries MBO at tens of millions of records per
    // symbol-day. A per-record `Warn` — a `std::to_string` plus a
    // `vector<Field>` plus a locked formatted write, each time — would not
    // merely be noisy: it would DOMINATE B2 and produce a plausible, wrong,
    // unattributable rows/sec that nothing in the harness would flag.
    //
    // So the flag is the mechanism, and it is load-bearing, not cosmetic.
    // Pinned by RecordIngressRefusalTest.MissingSymbolTableIsLoggedOnceNotPerRecord.
    if (!_warnedNoSymbolTable.exchange(true, std::memory_order_relaxed)) {
      gma::util::logger().log(
          gma::util::LogLevel::Warn,
          "Dispatcher: onRecord with no SymbolTable installed — every record "
          "on this path is being dropped (logged once per Dispatcher)",
          { {"symbolId", std::to_string(rec.header.symbolId)} });
    }
    return;
  }

  // THE SYMBOL COPY, and it is deliberate. `StreamValue` carries
  // `std::string symbol`, so the ticker has to come back at this boundary:
  // one `std::string` per record here, plus `StreamValue`'s own copy per
  // delivered value downstream. ENC-1481's interning removed the copy on the
  // INGRESS side only. Removing it from the value path is step 4 of
  // ENC-1487's five-step benchmark ("interned symbols") and has to stay
  // separately measurable — doing it here would merge two data points and
  // misattribute B2. So: expected, in scope as-is, and not to be "fixed"
  // without ENC-1487's numbers in hand.
  const std::string symbol = symbols->name(rec.header.symbolId);
  if (symbol.empty()) {
    // THREE ways in, not two. The third was missed by this comment's first
    // revision and is the one a reader would never guess:
    //
    //   1. `kInvalidSymbolId` — a record that names no symbol at all.
    //   2. An id this table never issued.
    //   3. An id the table DID issue, for the empty string.
    //      `SymbolTable::intern("")` is explicitly NOT special-cased
    //      (SymbolTable.hpp: an empty ticker is a provider bug, and folding it
    //      onto kInvalidSymbolId would hide it), so it returns a perfectly
    //      real, contains()-true id whose `name()` is "". `name()` flattens
    //      all three to the same answer, which is why ONE branch is correct
    //      here even though the causes differ.
    //
    // THE RULING: an empty ticker is a REFUSAL, not valid input. `SymbolTable`
    // is right to issue the id — that is how the provider bug stays visible at
    // the interning layer — and this is right to refuse it, because the engine
    // keys every listener, history buffer and `AtomicStore` entry on the
    // ticker STRING. Admitting "" would give every such record one shared
    // bucket that no subscription can name: values would be computed, stored
    // and delivered nowhere, which is the silent-wrong-answer failure the
    // null-table branch above also refuses. Route 3 previously fell through
    // here with no log whatsoever.
    //
    // Once per Dispatcher, for the same benchmark-integrity reason as above.
    if (!_warnedUnresolvedSymbol.exchange(true, std::memory_order_relaxed)) {
      gma::util::logger().log(
          gma::util::LogLevel::Warn,
          "Dispatcher: onRecord symbolId does not resolve to a ticker — record "
          "dropped (logged once per Dispatcher)",
          { {"symbolId", std::to_string(rec.header.symbolId)} });
    }
    return;
  }

  // Fields resolved by the normalizer — no DOM, no per-field allocation, no
  // name lookup by string hash. See include/gma/engine/RecordFieldSource.hpp
  // for the mapping table, which also *defines* "the equivalent tick" the
  // parity test drives down the JSON path.
  const engine::RecordFieldSource fields(rec);

  ingest(symbol, recordEventType(), fields);
}

} // namespace gma
