#pragma once
//
// THE SHIM — design sketch (ENC-1478, docs/data-layer.md §5).
// NOT IN THE BUILD. See data/README.md.
//
// The centerpiece. It makes ClickHouse behave as an indexed column store for
// gma's benefit, and gets rows into gma fast enough that gma's own compute is
// the bottleneck rather than the I/O.
//
// THREE PARTS, AND THE FIRST IS THE WHOLE DESIGN:
//
//   Planner  — analyses the graph's DATA DEPENDENCIES. It does NOT translate
//              the graph's MATH. That distinction is why this is safe and a
//              transpiler is not (docs §1.1).
//   Lint     — rejects any generated SELECT containing computation. A
//              structural guarantee, enforced at build time, replacing what a
//              two-engine parity diff could only have done with a tolerance.
//   Transport+decode — column blocks in, POD out, one conversion point.
//

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gma/Result.hpp"
#include "gma/data/IHistoryBackend.hpp"
#include "gma/data/Schema.hpp"


namespace gma::data {

// ---------------------------------------------------------------------------
// Planner
// ---------------------------------------------------------------------------

// One shard of work: a (symbol, day) tile. gma's semantics are already
// per-symbol, so shards are independent COMPLETE gma runs and need no
// cross-merge — which is what makes sharding semantically free and is the
// scaling story that preserves one implementation (docs §7).
struct Shard {
  std::string  symbol;
  std::int64_t dayStartNs = 0;
  std::int64_t dayEndNs   = 0;      // half-open
};

// What the planner derived from a manifest. Every field is DERIVED, not
// configured — see warmupNs in particular.
struct QueryPlan {
  std::vector<std::string> symbols;     // from the graph's Listener streamKeys
  TimeRange                requested;   // the caller's window
  TimeRange                effective;   // requested.fromNs - warmupNs

  // WARM-UP DEPTH IS DERIVED FROM THE GRAPH, NOT CONFIGURED.
  //
  // A graph containing sma_200 over 1-minute buckets needs 200 extra minutes
  // of input before the window start, or its first 200 output values are
  // wrong. The planner walks the graph for the deepest history requirement
  // across every operator and bucket period.
  //
  // This is the only place that number can be correct. A human setting it per
  // request gets it wrong silently, and in the direction of too little —
  // producing output that looks plausible and is warm-up garbage. It subsumes
  // what a `lookbackNs` config knob would otherwise carry.
  std::int64_t warmupNs = 0;

  // Column projection: the fields the manifest actually reads, resolved
  // through the connector's field map. A column store charges per column, so
  // `SELECT *` throws the entire layout away.
  std::vector<std::string> columns;

  // Which tables the graph touches at all.
  std::vector<RecordKind>  kinds;

  // (symbol, day) tiles over `effective`.
  std::vector<Shard>       shards;

  // TRUE when the graph contains a cross-symbol operator — an Aggregate /
  // JoinBy declared `by: "none"` (ENC-1292) — and therefore CANNOT be sharded
  // per symbol. Detected from the graph, never trusted from the caller
  // (docs §7, Q7). Sharding such a graph silently produces tuples that pair a
  // symbol with itself.
  bool crossSymbol = false;

  // One lossless SELECT per (kind, shard). Each must pass the lint.
  std::vector<Selection>   selections;
};

class Planner {
public:
  struct Options {
    std::int64_t shardDays = 1;
    // Refuse rather than silently widen when the graph's warm-up requirement
    // reaches past the ingested range (docs §12.3): a short warm-up is a
    // wrong answer with a plausible shape.
    bool         requireCompleteWarmup = true;
  };

  explicit Planner(Options opts);

  // `manifest` is the SAME JSON node graph TreeBuilder consumes. The planner
  // reads it as a logical plan's data requirements; it never interprets an
  // operator's arithmetic.
  gma::Result<QueryPlan> plan(std::string_view manifestJson,
                              std::span<const std::string> symbols,
                              TimeRange window) const;

private:
  Options opts_;
};

// ---------------------------------------------------------------------------
// Lint — the structural guarantee (docs §5.4)
// ---------------------------------------------------------------------------

// Parses a generated statement and rejects any construct that would move
// computation out of gma. A violation is a BUILD-TIME FAILURE, not a runtime
// warning.
//
// Checked over the whole request corpus (tests/treebuilder/corpus_requests.json,
// 272 entries), this asserts that NO manifest in the corpus can cause the
// planner to push computation down.
//
// ANTI-VACUITY IS PART OF THE CONTRACT. A lint that silently matched zero
// statements reads exactly like a passing one, which is the failure mode this
// repo has shipped before (ENC-1289's expected-failure machinery exists for
// the same reason). So: assert a non-zero statement count, and ship a
// deliberately-violating fixture registered WILL_FAIL.
struct LintViolation {
  std::string statement;
  std::string construct;      // e.g. "aggregate function 'avg'"
  std::string where;          // e.g. "projection list"
};

class SqlLint {
public:
  // Empty result == clean.
  static std::vector<LintViolation> check(std::string_view sql);

  // Convenience over a whole plan. Also asserts the plan produced at least
  // one statement.
  static std::vector<LintViolation> checkPlan(const QueryPlan& plan);
};

// Renders a Selection to SQL. Separate from the lint so the lint can be run
// against the rendered text rather than against the renderer's intent — the
// lint must see what the wire sees.
std::string renderSelect(const Selection& sel, std::string_view database);

// ---------------------------------------------------------------------------
// Transport + decode
// ---------------------------------------------------------------------------

// Interns symbol strings to ids. Exists because StreamValue carries
// `std::string symbol`, so today a symbol is HEAP-COPIED into every value on
// every edge of every DAG. At research scale that single copy is a measurable
// fraction of total cost (docs §5.3, §6).
class SymbolTable {
public:
  std::uint32_t intern(std::string_view sym);
  std::string_view lookup(std::uint32_t id) const;
  std::size_t size() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

struct ShimConfig {
  // Bounded buffering. ClickHouse reads faster than gma computes; without a
  // bound a month-long research scan buffers a month in RAM (docs §5.2).
  std::size_t  maxQueuedBlocks = 0;
  std::size_t  blockRows = 0;        // tuned to cache
  // Degree of shard parallelism. Per-shard execution is single-threaded by
  // the determinism contract (docs C7); parallelism is ACROSS shards, which
  // is why it costs nothing semantically.
  std::uint32_t shardConcurrency = 1;
};

// Drives a QueryPlan against an IL1Store and delivers decoded records.
//
// ONE CONVERSION POINT, COLUMN-WISE. No rapidjson::Document is constructed
// anywhere on this path; this is the Dispatcher::onRecord entry point (docs
// C2), and it is a net REDUCTION in per-tick work versus the shipped JSON
// ingress.
class Shim {
public:
  using TickFn = std::function<void(const TickRecord&)>;
  using BookFn = std::function<void(const BookRecord&)>;

  Shim(std::shared_ptr<IL1Store> store, ShimConfig cfg);
  ~Shim();

  // Executes every shard of the plan. Blocks until complete.
  //
  // Per-shard ordering is the physical primary key order — (symbol, ts_event,
  // provider_seq) — and the transport must not interleave parts in a way that
  // breaks it (docs §5.2 requirement 4).
  //
  // Refuses a plan whose `crossSymbol` is true unless shardConcurrency == 1.
  gma::Result<void> run(const QueryPlan& plan, TickFn onTick, BookFn onBook);

  SymbolTable& symbols() noexcept;

  struct Stats {
    std::uint64_t blocksRead   = 0;
    std::uint64_t rowsDecoded  = 0;
    std::uint64_t bytesRead    = 0;
    std::uint64_t shardsRun    = 0;
    std::uint64_t stallsOnBackpressure = 0;   // gma is the bottleneck: good
  };
  Stats stats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace gma::data
