#pragma once
//
// L1 row source — design sketch (ENC-1478, docs/data-layer.md §1, §8).
// NOT IN THE BUILD. See data/README.md.
//
// ============================================================================
// THIS INTERFACE SELECTS ROWS. IT DOES NOT COMPUTE VALUES.
// ============================================================================
//
// docs/data-layer.md §1 is the governing decision: ClickHouse is indexed,
// compressed, columnar STORAGE AND SELECTION, and gma performs 100% of the
// computation through one implementation of every operator.
//
// The reason is not architectural taste. A transpiler that reimplemented gma's
// math in SQL would produce small numerical differences — float accumulation
// order, bucket edge inclusivity, EMA seeding, gap handling — and this system
// exists to read differences of that size as signal. A discrepancy
// manufactured by the storage layer is indistinguishable from an indication,
// so there is no tolerance at which a second implementation is safe.
//
// Therefore the ONLY operations this interface may push into the store are
// lossless by construction — a row either satisfies a predicate or it does
// not, and no arithmetic happens on the way:
//
//     permitted : WHERE on symbol / ts_event / provider_seq
//                 partition pruning
//                 column projection
//                 ORDER BY matching the physical primary key
//                 LIMIT, AT THE QUERY-LAYER BOUNDARY ONLY (see below)
//
//     FORBIDDEN : any aggregate function    (avg, sum, min, max, count, argMin)
//                 any window function       (OVER, LAST_VALUE, exponentialMovingAverage)
//                 any expression in the projection list
//                 GROUP BY of any kind
//                 time bucketing            (toStartOfMinute, intDiv(ts, N))
//
// LIMIT HAZARD. A LIMIT is safe as a page cap on a RAW row request. It is NOT
// safe as a bound on a graph's input: it truncates the warm-up region and so
// changes every value the graph emits. The planner emits LIMIT only for a raw
// page, never for a computation feed.
//
// Enforcement is a LINT on the generated SQL (Shim.hpp, docs §5.4), not a
// parity diff. A lint fails at build time on the SHAPE of the query; a diff
// can only detect a numerical divergence after it has shipped, and across two
// engines it would need a tolerance — which is the noise floor we cannot
// accept.
//
// NO DERIVED LAYERS (docs §8.1). There is no L2, no rollup, no materialized
// bar table, no feature store. Research is ad hoc with no stable access
// pattern, so a derived layer is a bet on queries nobody has written; and
// under §1 any COMPUTED layer would be a second implementation besides.
// ClickHouse PROJECTIONS are allowed and are not a layer — same data, same
// columns, no new semantics, just a second physical sort order (docs §8.2).
//

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gma/data/Schema.hpp"

namespace arrow { class RecordBatch; }

namespace gma::data {

struct TimeRange;          // IProviderAdapter.hpp
struct CanonicalBatch;     // IProviderAdapter.hpp

// ---------------------------------------------------------------------------

struct BackendCapabilities {
  bool concurrentWriters = false;
  bool projections       = false;  // a second physical sort order (docs §8.2)
  bool skipIndexes       = false;
  bool snapshots         = false;  // serves latestSnapshotAtOrBefore (docs §8.5, Q9)
  bool idempotentReIngest= false;  // ReplacingMergeTree on a UNIQUE sort key (docs §8.6, C12)
  std::string describe;            // one line, for the startup log
};

// A PULL-based block stream, and pull is the point: gma computes slower than
// ClickHouse reads, so a push stream would deliver the whole window as fast as
// the wire allows and buffer a month in RAM. The consumer sets the pace
// (docs §5.2 requirement 3).
class IBlockStream {
public:
  virtual ~IBlockStream() = default;
  // std::nullopt when exhausted.
  virtual std::optional<CanonicalBatch> next() = 0;
  virtual void cancel() noexcept = 0;
};

// What rows to select. Deliberately NOT a query string and NOT an interval:
// there is no `interval` parameter anywhere in this interface, because an
// interval implies bucketing and bucketing is computation. Bars are produced
// by gma's TumblingWindow / BucketTime, from the rows this returns.
struct Selection {
  std::span<const std::string> symbols;
  TimeRange                    range;        // half-open [fromNs, toNs) on ts_event
  RecordKind                   kind = RecordKind::Trade;

  // Column projection, derived by the planner from the fields the manifest
  // actually reads (docs §5.1). Empty means every column of `kind`'s schema,
  // which on a column store is the expensive default and should be rare.
  std::span<const std::string> columns;

  // Raw-page cap only. MUST be nullopt for a computation feed — see the LIMIT
  // hazard above.
  std::optional<std::uint64_t> rawPageLimit;

  // Cursor for backward pagination of a raw page (the chart scroll-back
  // shape; feed-simulator's `before` parameter is the prior art).
  std::optional<std::int64_t>  beforeNs;
};

class IL1Store {
public:
  virtual ~IL1Store() = default;

  virtual std::string_view name() const noexcept = 0;
  virtual BackendCapabilities capabilities() const noexcept = 0;

  // READ. Rows ordered by (symbol, ts_event, provider_seq) — the physical
  // primary key, so no sort executes. Half-open on ts_event: a closed upper
  // bound re-delivers the boundary record at a splice, which is the
  // duplicate-bar bug the seq dedup exists to catch rather than to need.
  virtual std::unique_ptr<IBlockStream> select(const Selection& sel) = 0;

  // READ, the book case. The newest persisted snapshot at or before tsNs.
  // Only when capabilities().snapshots.
  //
  // This is the one piece of materialized derived state in the design and it
  // is argued rather than assumed (docs §8.5, Q9): without it, seeding a book
  // for an arbitrary window means replaying every delta from session open —
  // O(session) PER RESEARCH RUN. The argument for it is that a book snapshot
  // is raw book STATE, not an aggregate, and OrderBookManager is the single
  // implementation of applying deltas either way.
  virtual std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) = 0;

  // INGEST COMPLETENESS (docs §12.3). Returns true iff [fromNs, toNs) has been
  // fully ingested for this symbol.
  //
  // This exists because the three cases below are INDISTINGUISHABLE in a plain
  // SELECT, and the third silently returns a flat market:
  //     the market was quiet          -> no seq discontinuity; this is data
  //     the provider has a hole       -> seq discontinuity; a `gaps` row
  //     WE HAVE NOT INGESTED IT YET   -> nothing at all
  // A query over a window that is not complete must FAIL, not return a short
  // answer.
  virtual bool isComplete(std::string_view symbol, TimeRange range) const = 0;

  // WRITE (ingest side only; gma never writes the raw path). Insert
  // symbol-sorted batches per day partition: the sort key leads with symbol,
  // so raw DBN file order forces a per-part re-sort (docs §8.6).
  virtual void insert(const CanonicalBatch& batch) = 0;

  // Durability barrier: returns once everything inserted is visible to a
  // subsequent select().
  virtual void flush() = 0;
};

// ---------------------------------------------------------------------------
// ClickHouse L1 — THE store, not an optional backend
// ---------------------------------------------------------------------------
//
// docs/data-layer.md C10: with L1 in ClickHouse, history requires a service,
// which contradicts the brief's original "no new required services". Resolved
// as TWO DEPLOYMENT PROFILES rather than one compromise —
//     research / ingest : ClickHouse L1 is a required service, by design
//     client            : live-only, or reads a central L1 over the wire;
//                         no local store at all
// — and the dependency budget IMPROVES, because with no Parquet store there is
// no DuckDB (docs §15). Parquet and DuckDB are dropped from the design
// entirely; this file previously declared a ParquetHistoryBackend as the
// default and that is gone.
//
// DDL: data/schema/clickhouse.sql.
//
// TRANSPORT IS UNDECIDED AND IS BENCHMARK B1 (docs §5.2, §14.3). Native
// protocol speaks column blocks; HTTP speaks `FORMAT ArrowStream`. Either
// avoids row materialization and which is faster is unmeasured. This reverses
// an earlier lean toward HTTP: that was right when the backend was optional,
// and the shim's throughput is now the entire performance story.
class ClickHouseL1Store final : public IL1Store {
public:
  enum class Transport : std::uint8_t { Native = 0, HttpArrowStream = 1 };

  struct Config {
    std::string  endpoint;
    std::string  database = "gma";
    std::string  user;
    std::string  password;             // from env; never from an INI file
    Transport    transport = Transport::Native;   // pending B1
    bool         tls = false;
    std::size_t  insertBatchRows = 0;
    std::size_t  selectBlockRows = 0;  // tuned to cache, not convenience
    std::int32_t queryTimeoutMs = 0;
  };

  explicit ClickHouseL1Store(Config cfg);
  ~ClickHouseL1Store() override;

  std::string_view name() const noexcept override;      // "clickhouse-l1"
  BackendCapabilities capabilities() const noexcept override;

  std::unique_ptr<IBlockStream> select(const Selection& sel) override;
  std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) override;
  bool isComplete(std::string_view symbol, TimeRange range) const override;
  void insert(const CanonicalBatch& batch) override;
  void flush() override;

  // Applies data/schema/clickhouse.sql. Idempotent (every statement is
  // IF NOT EXISTS). Startup only; gma never issues DDL on a hot path.
  void ensureSchema();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------

// A fixture-backed store for CI: the replay-parity gate (docs §14.2) must run
// with no service. Reads the same canonical batches out of Arrow IPC files and
// honours the same Selection contract, including isComplete().
class FixtureL1Store final : public IL1Store {
public:
  explicit FixtureL1Store(std::string dir);
  ~FixtureL1Store() override;

  std::string_view name() const noexcept override;      // "fixture-l1"
  BackendCapabilities capabilities() const noexcept override;

  std::unique_ptr<IBlockStream> select(const Selection& sel) override;
  std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) override;
  bool isComplete(std::string_view symbol, TimeRange range) const override;
  void insert(const CanonicalBatch& batch) override;
  void flush() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Unknown kind is an error at startup, never a silent fallback: a deployment
// that asked for the shared store and quietly got a fixture directory is worse
// than one that refuses to boot.
std::shared_ptr<IL1Store> makeL1Store(
    std::string_view kind,
    const std::unordered_map<std::string, std::string>& params);

} // namespace gma::data
