#pragma once
//
// Sinks (gma -> history) — design sketch (ENC-1478, docs/data-layer.md §7).
// NOT IN THE BUILD. See data/README.md.
//
// OPTIONAL IN THE STRICT SENSE: no sink in a graph means no history dependency
// at runtime, and a build without gma_data still produces a working gma_server.
//
// THREE USES (docs §11):
//
//   * RESEARCH OUTPUT — the headless path. ReplaySource over L1 -> DAG ->
//     SinkNode -> Arrow table. This is "gma as the query engine transforming
//     tables", and it is the primary use.
//   * RecordingSink — the parity instrument (bottom of this file).
//   * DERIVED-SERIES PERSISTENCE, as an OPTIONAL CACHE. Note this is no longer
//     load-bearing: under docs §1 the recompute path is the same code through
//     the same operators, so a persisted copy is a cache and not a correctness
//     requirement. If added, it MUST be invalidated by graph identity — a
//     persisted sma20 from a changed manifest is a wrong answer with a
//     plausible shape (docs Q11).
//
// NEVER the raw path: raw durability is the ingest job's, upstream of gma, and
// gma owning it is what docs §4 rule 3 forbids. A SinkNode writing raw ticks
// would make a gma crash a data-loss event.
//

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "gma/data/IHistoryBackend.hpp"
#include "gma/data/Schema.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/server/RequestKey.hpp"

namespace arrow { class Schema; }

namespace gma::data {

// Where a sink's rows go. Separated from IL1Store so a sink can write to
// a plain Arrow IPC file (the parity fixture) without a backend configured.
class IHistorySink {
public:
  virtual ~IHistorySink() = default;
  virtual void append(const CanonicalBatch& batch) = 0;
  virtual void flush() = 0;
  virtual std::uint64_t rowsWritten() const noexcept = 0;
};

std::shared_ptr<IHistorySink> makeIngestSink(std::shared_ptr<IL1Store>);
std::shared_ptr<IHistorySink> makeArrowIpcFileSink(std::string path,
                                                   std::shared_ptr<arrow::Schema>);

// ---------------------------------------------------------------------------

struct SinkConfig {
  // Flush triggers. Whichever fires first.
  std::size_t  maxRows = 0;
  // EVENT-time interval, not wall clock. A sink that flushed on a wall-clock
  // timer would produce different batch boundaries under replay than under
  // live, for identical input — which is the same class of defect as a
  // wall-clock window node (docs C1), one layer out.
  std::int64_t flushIntervalNs = 0;
  // Flush on shutdown. Default on: the alternative is losing the tail batch of
  // every session.
  bool         flushOnShutdown = true;
};

// A terminal node. Registered on engine::NodeTypeRegistry as "Sink" so it is
// specifiable from a JSON request tree exactly like a Responder, and
// TreeBuilder needs no special case.
//
// Accumulates emitted StreamValues into an Arrow builder and flushes a
// RecordBatch on a size or event-time trigger. shutdown() flushes.
class SinkNode final : public INode {
public:
  SinkNode(std::shared_ptr<IHistorySink> sink,
           std::shared_ptr<arrow::Schema> schema,
           SinkConfig cfg);
  ~SinkNode() override;

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  std::uint64_t rowsAccepted() const noexcept;
  std::uint64_t batchesFlushed() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// RecordingSink — the parity instrument
// ---------------------------------------------------------------------------
//
// Same mechanism, fixed schema, and it exists because the WebSocket boundary
// CANNOT measure parity (docs/data-layer.md C5). ClientSession's outbox
// coalesces `update` frames above 256 queued and displaces the oldest pending
// one at 4096 (ENC-996 / ENC-1072); docs/ARCHITECTURE.md §8 says in as many
// words that a consumer doing event replay cannot rely on that stream. A replay
// outrunning the harness would therefore drop frames BY DESIGN and the diff
// would report a mismatch that is not a defect.
//
// So the recording is taken at the Responder's send function — upstream of the
// outbox, no queue, no coalescing, every value.
//
// Schema (the parity diff key, docs §10.2 step 5):
//     request_key     utf8        rendered RequestKey (int or string)
//     symbol          utf8
//     event_time_ns   int64       StreamValue::eventTimeNs  (added by C1)
//     bucket_start_ms int64       StreamValue::bucketStartMs, 0 == unbucketed
//     value           utf8        canonical rendering, incl. Record terminals
//
// `value` is text rather than a double because ArgType is a variant over bool,
// int, double, string, vector<int>, vector<double>, vector<ArgValue> and
// Record — the corpus has Record terminals — and a parity diff must compare
// ALL of them exactly. A float column would quietly convert the comparison
// into an approximate one, which is the thing §10 refuses.
class RecordingSink final {
public:
  RecordingSink(std::string path);
  ~RecordingSink();

  // Install as the Responder's send function for a request. Returns the
  // callable the Responder captures, so recording needs no Responder change.
  std::function<void(const StreamValue&)> forRequest(const server::RequestKey& key);

  void flush();
  std::uint64_t rowsWritten() const noexcept;

  static std::shared_ptr<arrow::Schema> schema();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gma::data
