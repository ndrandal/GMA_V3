#pragma once
//
// Canonical L1 Arrow schemas, and the one codec between them and the engine's
// PODs (ENC-1481).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D3, D7, D8;
//         docs/data-layer.md §5.3, §8.3, §8.4.
//
// TWO REPRESENTATIONS, DELIBERATELY:
//
//   * arrow::Schema — the wire/storage form. Everything upstream of the engine
//     (adapter -> normalizer -> L1 -> replay) moves record BATCHES in this
//     shape. No row objects, no JSON DOM, no per-field conversion.
//
//   * TickRecord / BookRecord / SnapshotRecord — plain PODs, the engine-facing
//     form, defined in `include/gma/data/Record.hpp` (gma_engine). The decode
//     below turns a batch into these COLUMN-WISE, once per batch. That decode
//     is the single Arrow->scalar boundary in the system
//     (docs/data-layer.md C2). It exists because gma's DAG is a scalar push
//     pipeline — ArgType is a scalar variant and INode::onValue takes one
//     StreamValue — and making it columnar is a rewrite of ~25 node types that
//     this design explicitly does not take on.
//
// THIS HEADER IS WHERE ARROW STOPS. It is reachable only from `gma_data`;
// `gma_engine` neither includes it nor links the library (SPEC D14 — the
// live-only variant of the client profile omits `gma_data` entirely and must
// still link; D14's other client variant reads a central L1 over the wire and
// does need this header). The invariant is asserted at build time by
// `tools/check_engine_isolation.py`, run as the ctest case
// `gma_engine_isolation`.
//
// WHAT IS NOT HERE: the shim's planner, the SQL lint, transport, the DBN
// adapter and `Dispatcher::onRecord`. Those are ENC-1482 / ENC-1485 /
// ENC-1486. This is the schema and the codec, nothing more.
//

#include "gma/data/Record.hpp"
#include "gma/data/SymbolTable.hpp"

#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace arrow { class MemoryPool; }

namespace gma::data {

// ---------------------------------------------------------------------------
// Schemas (docs/data-layer.md §8.3, §8.4)
// ---------------------------------------------------------------------------
//
// Common columns on EVERY table, in this order:
//
//   ts_event       timestamp(NANO, "UTC")   venue event time. THE ONLY TIME AN
//                                           OPERATOR MAY KEY OFF (SPEC D9).
//   ts_recv        timestamp(NANO, "UTC")   normalizer receipt. Diagnostics and
//                                           latency only — never an operator
//                                           input, never a window boundary.
//   provider_seq   uint64, NON-NULL         monotonic within
//                                           (session_id, symbol). Synthesized
//                                           by the normalizer when the provider
//                                           has none. Non-null is the point:
//                                           the splice's dedup predicate has no
//                                           "what if absent" branch.
//   session_id     uint32                   normalizer session. Makes
//                                           provider_seq comparable across a
//                                           restart; the dedup key is the PAIR.
//   symbol         dictionary<int32, utf8>  THE TICKER (SPEC D7). Thousands of
//                                           values against millions of rows.
//   instrument_id  uint32                   the DBN id, retained for
//                                           provenance (SPEC D7). Not a key:
//                                           it is dataset-scoped and
//                                           reassignable, and keying L1 on it
//                                           would make every query a join.
//   venue          dictionary<int16, utf8>  Nasdaq / OTC / … A handful of
//                                           values, hence int16 indices.
//
// EVERY COLUMN IS NON-NULLABLE. Not tidiness: a nullable column is a branch on
// every read, and the two fields that could plausibly be absent — provider_seq
// and ts_recv — are synthesized by the normalizer precisely so that nothing
// downstream carries an "if absent" path.
//
// Three tables rather than one union table: a unified event table needs every
// type's fields nullable, which defeats the codecs and makes every read scan
// columns it does not want.

std::shared_ptr<arrow::Schema> tradesSchema();          // §8.4
std::shared_ptr<arrow::Schema> bookEventsSchema();      // §8.4  (L2/L3 deltas)
std::shared_ptr<arrow::Schema> bookSnapshotsSchema();   // §8.5

// `quotes` (L1 top-of-book) is NOT POPULATED IN THE FIRST SLICE — there is no
// mbp-1 in the Databento plan, so top-of-book is computed from the
// reconstructed book via OrderBookManager::bestBid/bestAsk, which is correct
// under SPEC D1 (docs/data-layer.md §8.4). The shape is defined so
// feed-simulator and future L1-only providers have somewhere to land, and so
// `schemaFor(RecordKind::Quote)` is not a hole.
std::shared_ptr<arrow::Schema> quotesSchema();

// Returns nullptr for an unrecognized kind rather than throwing — callers
// decoding a batch of unknown provenance get a checkable answer.
std::shared_ptr<arrow::Schema> schemaFor(RecordKind kind);

// Schema identity, written into every Arrow IPC stream's key-value metadata.
// A reader that meets an unknown version refuses the stream rather than
// guessing a column layout.
inline constexpr std::string_view kSchemaVersionKey = "gma.schema_version";
inline constexpr std::int32_t     kSchemaVersion    = 1;

// The version string carried in the metadata of every schema above, parsed
// back out by `schemaVersionOf`. Returns -1 when the key is absent or
// unparseable — "this stream does not say", which is not the same as version 0.
std::int32_t schemaVersionOf(const arrow::Schema& schema);

// ---------------------------------------------------------------------------
// Encode — PODs to one RecordBatch, column-wise
// ---------------------------------------------------------------------------
//
// Vector in, batch out, rather than an append-as-you-go builder object: both
// producers are batch-shaped (the DBN adapter decodes a provider block, the
// L1 writer inserts a block), so a per-record builder would be an interface
// with no caller. The dictionary columns are built from the ids the records
// carry, resolved to text ONCE PER DISTINCT ID through the tables — never per
// row.
//
// `symbols` and `venues` must contain every id the records name; an id neither
// table knows is an error, not an empty string, because a silently-empty
// ticker in L1 is indistinguishable from a provider gap.

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeTrades(
    const std::vector<TickRecord>& records,
    const SymbolTable& symbols, const SymbolTable& venues,
    arrow::MemoryPool* pool = nullptr);

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeQuotes(
    const std::vector<TickRecord>& records,
    const SymbolTable& symbols, const SymbolTable& venues,
    arrow::MemoryPool* pool = nullptr);

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeBookEvents(
    const std::vector<BookRecord>& records,
    const SymbolTable& symbols, const SymbolTable& venues,
    arrow::MemoryPool* pool = nullptr);

arrow::Result<std::shared_ptr<arrow::RecordBatch>> encodeBookSnapshots(
    const std::vector<SnapshotRecord>& records,
    const SymbolTable& symbols, const SymbolTable& venues,
    arrow::MemoryPool* pool = nullptr);

// ---------------------------------------------------------------------------
// Decode — THE Arrow->scalar boundary (docs/data-layer.md §5.3, C2)
// ---------------------------------------------------------------------------
//
// Column-wise: each column's typed array is fetched once and then walked, so
// there is no per-field type dispatch and no DOM. The dictionary columns are
// interned ONCE PER DICTIONARY ENTRY and the per-row work is an array index —
// which is the whole point of the uint32 ids in RecordHeader (SPEC D3).
//
// The batch's schema is checked against the expected one first, so decoding a
// book batch as trades fails loudly instead of reading `action` as a price.
// `out` is appended to, not cleared, so a caller may accumulate across batches
// — and a REFUSED batch appends nothing. The two go together: a decode that
// left a partial prefix behind would make accumulate-then-check-the-Status
// silently wrong, and these functions refuse mid-loop as a matter of course
// (an out-of-domain enum byte, a null in a dictionary) rather than only on
// corrupt memory.
//
// What the batch is checked for, beyond its schema: internal consistency
// (arrow::RecordBatch::Validate — column lengths against num_rows, which is
// every decoder's loop bound), no nulls in any column or in a dictionary's
// values, and every enum-valued column inside its declared domain. A batch
// off ClickHouse `FORMAT ArrowStream` is a wire input; its column TYPES can be
// exactly canonical while its contents are not representable.

arrow::Status decodeTicks(const arrow::RecordBatch& batch, RecordKind kind,
                          SymbolTable& symbols, SymbolTable& venues,
                          std::vector<TickRecord>& out);

arrow::Status decodeBookEvents(const arrow::RecordBatch& batch,
                               SymbolTable& symbols, SymbolTable& venues,
                               std::vector<BookRecord>& out);

arrow::Status decodeBookSnapshots(const arrow::RecordBatch& batch,
                                  SymbolTable& symbols, SymbolTable& venues,
                                  std::vector<SnapshotRecord>& out);

} // namespace gma::data
