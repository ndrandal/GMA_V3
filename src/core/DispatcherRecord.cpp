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
//   * this file names none of `rapidjson`, `Document`, `payload`,
//     `HasMember`, `MemberBegin`, `GetDouble`, `JsonFieldSource`, `const
//     Event&`;
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
    // than a loud nothing. Logged at Warn, not Error, because the only way to
    // reach it is a wiring mistake that the first record reports.
    gma::util::logger().log(gma::util::LogLevel::Warn,
                            "Dispatcher: onRecord with no SymbolTable installed",
                            { {"symbolId", std::to_string(rec.header.symbolId)} });
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
    // Either kInvalidSymbolId (a record that names no symbol) or an id this
    // table never issued. `onTick` has always dropped an empty stream key;
    // this is the same rule reached from the other side.
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
