#pragma once
//
// Provider adapter + normalizer — design sketch (ENC-1478, docs/data-layer.md §3).
// NOT IN THE BUILD. See data/README.md.
//
// Two interfaces, split on one line: the ADAPTER knows a vendor's protocol and
// nothing about storage; the NORMALIZER knows the canonical schema, owns
// durability, and knows nothing about any vendor.
//
// Relationship to the EXISTING gma::feed::IFeedAdapter
// (connectors/market/include/gma/feed/IFeedAdapter.hpp): that interface is
// `std::vector<FeedEvent> translate(const std::string& rawMessage)` — one
// std::string in, a heap vector of variants out, per message. It is the right
// shape for a line-oriented JSON feed and the wrong shape for a binary batched
// one (DBN), where the cost is dominated by the allocation per message. This
// interface is batch-in / columns-out and is NOT a rename of it; the two
// coexist, with IFeedAdapter remaining the legacy `market.wsclient` path
// (docs/data-layer.md C2).
//

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gma/data/Schema.hpp"

namespace arrow { class RecordBatch; }

namespace gma::data {

// ---------------------------------------------------------------------------
// Provider adapter
// ---------------------------------------------------------------------------

struct ProviderCapabilities {
  bool live             = false;   // can stream
  bool history          = false;   // can serve a past range (some providers can't)
  bool l1               = false;
  bool l2               = false;
  bool l3               = false;   // per-order (TotalView, ITCH)
  bool bookSnapshots    = false;   // can serve a snapshot on demand -> re-snapshot on gap
  // False means the normalizer must synthesize provider_seq (docs §4.4). This
  // is the one capability the splice's correctness depends on, which is why it
  // is reported rather than assumed.
  bool monotonicSeq     = false;
  // True when seq is monotonic per channel rather than per symbol. Still a
  // total order, so still exact for dedup.
  bool seqIsGlobal      = false;
  std::int64_t maxHistoryLookbackNs = 0;  // 0 == unknown / unlimited
};

// Half-open event-time range [fromNs, toNs). Half-open everywhere in this
// design: a closed upper bound double-delivers the boundary record on a splice,
// which is exactly the duplicate-bar bug the seq dedup exists to avoid.
struct TimeRange {
  std::int64_t fromNs = 0;
  std::int64_t toNs   = 0;        // 0 == open-ended (live)
};

// One decoded, canonical batch plus what kind of rows it holds. Batches are
// single-kind: a batch mixing trades and book events would need the union
// schema §5 rejects.
struct CanonicalBatch {
  RecordKind                          kind = RecordKind::Trade;
  std::shared_ptr<arrow::RecordBatch> batch;
};

using BatchSink = std::function<void(CanonicalBatch)>;

// A provider's protocol, and only that. No storage, no Arrow IPC, no sequence
// synthesis — those are the normalizer's.
//
// Implementations: DatabentoAdapter (DBN; docs Q15 on client-vs-decode),
// FeedSimAdapter (the local default feed and the parity fixture source),
// IqFeedAdapter (the one being retired).
class IProviderAdapter {
public:
  virtual ~IProviderAdapter() = default;

  virtual std::string_view name() const noexcept = 0;
  virtual ProviderCapabilities capabilities() const noexcept = 0;

  // Decode a chunk of raw bytes into zero or more canonical batches.
  //
  // Byte-span rather than std::string: a DBN frame is binary and may be split
  // arbitrarily across reads, so the adapter owns reassembly and may retain a
  // partial record between calls. Returning batches by value (shared_ptr
  // inside) rather than through the sink keeps the adapter free of threading
  // assumptions.
  virtual std::vector<CanonicalBatch> decode(std::span<const std::byte> raw) = 0;

  // Flush any record the adapter is holding because its frame was incomplete.
  // Called on disconnect: a partial record at a disconnect is a GAP, and the
  // adapter must report it rather than hold it until a reconnect silently
  // completes it with bytes from a different session.
  virtual std::vector<CanonicalBatch> flushPartial() = 0;

  // Protocol-level subscribe/unsubscribe payloads. Mirrors the existing
  // IFeedAdapter::subscribeMessage, generalised to bytes.
  virtual std::vector<std::byte> subscribeFrame(
      std::span<const std::string> symbols, bool l2) = 0;

  // Request a fresh book snapshot. Only meaningful when
  // capabilities().bookSnapshots. This is the LIVE half of the gap-recovery
  // callback the data layer installs on
  // OrderBookManager::requestSnapshotFn (docs §4.6 step 3); the replay half
  // reads IL1Store::latestSnapshotAtOrBefore instead.
  virtual std::vector<std::byte> snapshotRequestFrame(std::string_view symbol) = 0;

  // Historical fetch, when the provider serves one. Providers that do not
  // (capabilities().history == false) force history to come from our own
  // store, which is the normal case and the reason the store exists.
  virtual void fetchHistory(std::span<const std::string> symbols,
                            TimeRange range,
                            RecordKind kind,
                            const BatchSink& out) = 0;
};

// ---------------------------------------------------------------------------
// Normalizer
// ---------------------------------------------------------------------------

struct NormalizerConfig {
  std::uint32_t sessionId = 0;            // bumped every process start (docs §8.4)
  std::string   liveTailPath;             // Arrow IPC stream file (bounded ring)
  std::int64_t  liveTailBytes = 0;        // 0 == no tail (not recommended)
  std::size_t   sinkBatchRows = 0;        // flush to history at N rows
  std::int64_t  sinkFlushIntervalNs = 0;  // ...or N ns of EVENT time, whichever first
  bool          synthesizeSeq = true;     // when !capabilities().monotonicSeq
  bool          recordGaps = true;        // docs Q9
};

// Owns durability. Everything gma reads was written here, and gma writing
// nothing on the raw path is the point (docs §3 rule 1): a gma crash loses no
// raw data because gma never held it.
//
// Three outputs from one input:
//   1. the live tail   — Arrow IPC stream, record-at-a-time, crash-readable;
//                        what LiveSource/SplicedSource tail.
//   2. the history sink — batched, to IHistorySink -> IHistoryBackend.
//   3. the in-process fan-out to any attached BatchSink (the live path, when
//      the source is in-process and does not need to go through the tail).
class INormalizer {
public:
  virtual ~INormalizer() = default;

  // Assign session, synthesize provider_seq where needed, stamp ts_recv,
  // detect sequence gaps, then write to all three outputs. Returns the number
  // of rows admitted (it may differ from the input on a dedup or a reject).
  virtual std::size_t ingest(CanonicalBatch batch) = 0;

  // Event time through which this symbol has been FLUSHED TO HISTORY.
  //
  // This exists for one reason and it is the reason named in docs §8.1: a
  // backfill that returns short must be able to distinguish "there is no data
  // in that window" from "the data is not written yet". Without it, a
  // reconnect-splice silently serves a thin window and the chart is wrong with
  // no error anywhere.
  virtual std::int64_t flushedThroughNs(std::string_view symbol) const = 0;

  // Force a flush, up to and including everything buffered. Called by a source
  // that is waiting on flushedThroughNs to reach its splice point.
  virtual void flush() = 0;

  // Attach an in-process consumer of canonical batches (the live fan-out).
  virtual std::uint64_t attach(BatchSink sink) = 0;
  virtual void detach(std::uint64_t token) = 0;

  struct Stats {
    std::uint64_t rowsIn        = 0;
    std::uint64_t rowsAdmitted  = 0;
    std::uint64_t seqGaps       = 0;   // docs §8.2
    std::uint64_t seqSynthesized= 0;
    std::uint64_t outOfOrder    = 0;   // within a symbol: a provider or our bug
    std::uint64_t partialFrames = 0;   // flushPartial() at a disconnect
  };
  virtual Stats stats() const = 0;
};

} // namespace gma::data
