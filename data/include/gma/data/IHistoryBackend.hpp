#pragma once
//
// History backend — design sketch (ENC-1478, docs/data-layer.md §6).
// NOT IN THE BUILD. See data/README.md.
//
// FOUR OPERATIONS AND NOTHING ELSE. The interface is narrow on purpose: it is
// the contract that lets a deployment swap Parquet-on-disk for a shared
// ClickHouse cluster with NO gma code change, and every member added to it is
// another thing both backends have to mean the same way.
//
// gma code does not branch on which backend it has. capabilities() exists for
// OPERATOR-FACING STARTUP VALIDATION only — the two backends genuinely differ
// on concurrent writers and retention, and the alternative to reporting the
// difference is pretending it away and having the splice behave differently per
// deployment.
//

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <string_view>
#include <vector>

#include "gma/data/Schema.hpp"

namespace arrow { class RecordBatch; }

namespace gma::data {

struct TimeRange;          // IProviderAdapter.hpp
struct CanonicalBatch;     // IProviderAdapter.hpp

// ---------------------------------------------------------------------------

struct BackendCapabilities {
  bool concurrentWriters   = false;  // Parquet: false. ClickHouse: true.
  bool serverSidePredicate = false;  // pushdown beyond partition pruning
  bool retentionPolicy     = false;  // TTL managed by the store
  bool snapshots           = false;  // serves latestSnapshotAtOrBefore
  bool transactional       = false;  // an append is all-or-nothing
  std::string describe;              // one line, for the startup log
};

// A pull-based batch stream. Pull rather than a callback because a ReplaySource
// must control the rate at which it feeds the engine — a push stream from
// DuckDB would deliver the whole window as fast as it can read, and the
// deterministic replay (one ingress thread, drain at the end) needs to consume
// at its own pace.
class IBatchStream {
public:
  virtual ~IBatchStream() = default;
  // Returns nullptr-batch when exhausted.
  virtual std::optional<CanonicalBatch> next() = 0;
  virtual void cancel() noexcept = 0;
};

// ---------------------------------------------------------------------------

class IHistoryBackend {
public:
  virtual ~IHistoryBackend() = default;

  virtual std::string_view name() const noexcept = 0;
  virtual BackendCapabilities capabilities() const noexcept = 0;

  // READ. Half-open [range.fromNs, range.toNs) on ts_event, ordered by
  // (symbol, ts_event, provider_seq) — the same order the ClickHouse MergeTree
  // key and the Parquet within-file sort use, so neither backend has to sort to
  // answer.
  //
  // Half-open is not a detail: a closed upper bound re-delivers the boundary
  // record on a splice, which is the duplicate-bar bug the seq dedup exists to
  // catch rather than to need.
  virtual std::unique_ptr<IBatchStream> query(
      std::span<const std::string> symbols,
      TimeRange range,
      RecordKind kind) = 0;

  // READ, the book case. The newest persisted snapshot at or before tsNs.
  //
  // This is what makes an arbitrary-window L2 replay affordable: without it,
  // seeding a book means replaying every delta from session open, O(session)
  // per backtest (docs §4.6, Q6 on cadence).
  virtual std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) = 0;

  // WRITE. Batched; the caller decides the batch size. A backend that cannot
  // accept concurrent appends says so in capabilities() and the normalizer
  // serialises.
  virtual void append(const CanonicalBatch& batch) = 0;

  // Durability barrier. Returns once everything appended so far is readable by
  // a subsequent query() — which is precisely what
  // INormalizer::flushedThroughNs is reporting on, and what a splice waits for
  // rather than reading a short window as an empty one (docs §8.1).
  virtual void flush() = 0;
};

// ---------------------------------------------------------------------------
// Default backend: Parquet on disk, queried through embedded DuckDB
// ---------------------------------------------------------------------------
//
// THE DEFAULT, and the reason "no new required services in the default
// deployment" holds: no server, no daemon, no port. DuckDB reads Parquet
// natively and returns Arrow, so the read path has no row-by-row conversion
// either.
//
// Layout mirrors the ClickHouse ORDER BY so the two answer the same query the
// same way:
//     <root>/<kind>/symbol=<SYM>/date=<YYYY-MM-DD>/<session>-<seq>.parquet
// with rows sorted by (ts_event, provider_seq) inside each file, so DuckDB
// prunes on the path AND on the row group.
//
// OPEN: whether DuckDB is justified at all. It is the largest single dependency
// in the default deployment, and Arrow's own Dataset API may cover the
// predicate pushdown this interface needs. Measure before vendoring —
// docs/data-layer.md §11, Q14.
class ParquetHistoryBackend final : public IHistoryBackend {
public:
  struct Config {
    std::string  root;                      // directory; created if absent
    std::string  compression = "zstd";
    std::int32_t compressionLevel = 3;
    std::size_t  rowGroupRows = 0;          // 0 == Arrow default
    // Sort each batch by symbol before writing. Keeps one file per
    // (symbol, date) rather than a file per batch per symbol, which is the
    // difference between pruning and a full scan.
    bool         sortBySymbolOnAppend = true;
  };

  explicit ParquetHistoryBackend(Config cfg);
  ~ParquetHistoryBackend() override;

  std::string_view name() const noexcept override;        // "parquet+duckdb"
  BackendCapabilities capabilities() const noexcept override;

  std::unique_ptr<IBatchStream> query(std::span<const std::string> symbols,
                                      TimeRange range,
                                      RecordKind kind) override;
  std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) override;
  void append(const CanonicalBatch& batch) override;
  void flush() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Optional backend: clickhouse-server
// ---------------------------------------------------------------------------
//
// STUB. For deployments needing a shared, concurrent, TB-scale store. NOT in
// the default deployment and not a required service.
//
// DDL: data/schema/clickhouse.sql — MergeTree, PARTITION BY
// toYYYYMMDD(ts_event), ORDER BY (symbol, ts_event, provider_seq),
// DateTime64(9,'UTC'), DoubleDelta on ts/seq, Delta+ZSTD on prices.
//
// TRANSPORT IS UNDECIDED (docs Q7): clickhouse-cpp native, or HTTP with
// FORMAT ArrowStream. HTTP costs some throughput and saves a dependency; given
// the backend is optional, HTTP is the leaning. The enum below exists so the
// decision is visible rather than buried in a constructor.
//
// NOT a streaming tail. ClickHouse is a store, not a bus — the live path is
// always the normalizer's Arrow IPC tail (docs §9, deferred).
class ClickHouseHistoryBackend final : public IHistoryBackend {
public:
  enum class Transport : std::uint8_t { Native = 0, HttpArrowStream = 1 };

  struct Config {
    std::string  endpoint;                  // host:port or URL
    std::string  database = "gma";
    std::string  user;
    std::string  password;                  // from env; never from an INI file
    Transport    transport = Transport::HttpArrowStream;
    bool         tls = false;
    std::size_t  insertBatchRows = 0;
    std::int32_t queryTimeoutMs = 0;
  };

  explicit ClickHouseHistoryBackend(Config cfg);
  ~ClickHouseHistoryBackend() override;

  std::string_view name() const noexcept override;        // "clickhouse"
  BackendCapabilities capabilities() const noexcept override;

  std::unique_ptr<IBatchStream> query(std::span<const std::string> symbols,
                                      TimeRange range,
                                      RecordKind kind) override;
  std::optional<CanonicalBatch> latestSnapshotAtOrBefore(
      std::string_view symbol, std::int64_t tsNs) override;
  void append(const CanonicalBatch& batch) override;
  void flush() override;

  // Applies data/schema/clickhouse.sql. Idempotent (every statement is
  // IF NOT EXISTS). Startup-time only; gma never issues DDL on a hot path.
  void ensureSchema();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------

// Factory from config, so the backend choice is an INI key and nothing more.
// Unknown kind is an error at startup, not a silent fallback to the default —
// a deployment that asked for the shared store and quietly got a local
// directory is worse than one that refuses to boot.
std::shared_ptr<IHistoryBackend> makeHistoryBackend(
    std::string_view kind,
    const std::unordered_map<std::string, std::string>& params);

} // namespace gma::data
