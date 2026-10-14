#pragma once
//
// Sources — design sketch (ENC-1478, docs/data-layer.md §4).
// NOT IN THE BUILD. See data/README.md.
//
// THE SPLICE LIVES HERE AND NOWHERE ELSE. No graph, node, computer or sink
// contains a branch on "am I live" — that is the whole deliverable. A DAG built
// by TreeBuilder cannot tell which of the three sources is driving it.
//
// Relationship to the EXISTING engine seam: gma::engine::IIngressSource
// (include/gma/engine/IngressRegistry.hpp) is start()/stop() and is exactly
// right for an endless live feed. It is wrong for a replay, which FINISHES, and
// it carries no clock. So ISource is a gma_data interface with the two extra
// members, and SourceIngress (bottom of this file) adapts it to
// IIngressSource so wiring stays an INI edit against cfg.ingress[] and needs no
// main.cpp change (ENC-31). docs/data-layer.md C7.
//

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gma/data/IHistoryBackend.hpp"
#include "gma/data/Shim.hpp"
#include "gma/data/IProviderAdapter.hpp"
#include "gma/data/Schema.hpp"
#include "gma/engine/IngressRegistry.hpp"

namespace gma {
class Dispatcher;
class OrderBookManager;
}

namespace gma::data {

// ---------------------------------------------------------------------------
// Source mode and state
// ---------------------------------------------------------------------------

// Whether history replayed through the DAG produces OUTPUT or only advances
// STATE. These are genuinely different operations and the engine has neither
// today (docs/data-layer.md C4, Q3).
//
//   Suppress — prime MarketTickComputer::_symbolHistories and
//              Dispatcher::_histories, deliver nothing to listeners. What a
//              live warm-up wants: a client subscribing to a 200-period SMA
//              should receive its FIRST value already warm, not 200
//              partially-warm ones.
//   Emit     — deliver everything. What a backtest and the parity test want.
enum class ReplayMode : std::uint8_t { Suppress = 0, Emit = 1 };

// docs/data-layer.md §4.1. PER SYMBOL, not per source — see SpliceStatus.
enum class SpliceState : std::uint8_t {
  Buffering    = 0,  // live subscription open, buffering, nothing to the engine
  Backfilling  = 1,  // querying history up to the splice point
  Replaying    = 2,  // feeding history into the engine in event-time order
  Draining     = 3,  // feeding the buffer, dropping seq <= lastReplayedSeq
  Live         = 4,  // pass-through
  Resync       = 5,  // gap or disconnect — re-enter Backfilling
  Failed       = 6,  // splice abandoned; see SpliceStatus::detail
};

struct SpliceStatus {
  std::string   symbol;
  SpliceState   state = SpliceState::Buffering;
  std::uint64_t lastReplayedSeq = 0;
  std::uint32_t lastReplayedSession = 0;   // dedup key is the PAIR (docs §8.4)
  std::int64_t  lastDeliveredEventTimeNs = 0;
  std::size_t   bufferedRecords = 0;
  std::uint32_t spliceAborts = 0;          // docs §4.3 — an operator signal
  std::string   detail;                    // non-empty iff Failed
};

// ---------------------------------------------------------------------------
// ISource
// ---------------------------------------------------------------------------

// Where a source delivers. Both are the engine's own entry points; a source
// never touches AtomicStore or a node directly.
struct SourceTargets {
  // Dispatcher::onRecord — the NON-JSON ingress path this design adds
  // (docs/data-layer.md C2). Fed by the one column-wise decode of an Arrow
  // batch into TickRecord. No rapidjson::Document is constructed anywhere on
  // this path.
  Dispatcher*       dispatcher = nullptr;
  // Book mutations go to the EXISTING manager, through its existing entry
  // points, gated by its existing onSeq()/onReset() (docs §4.6). The data
  // layer drives that machinery; it does not reimplement it.
  OrderBookManager* books = nullptr;
};

class ISource {
public:
  virtual ~ISource() = default;

  virtual std::string_view name() const noexcept = 0;

  // ---- lifecycle --------------------------------------------------------
  //
  // start() returns as soon as the source is running; it does NOT wait for a
  // splice to complete. A caller that needs the graph warm before it reports
  // ready waits on awaitReady().
  virtual void start() = 0;
  virtual void stop() noexcept = 0;

  // TRUE WHEN THE SOURCE WILL PRODUCE NOTHING FURTHER.
  //
  // This is the member IIngressSource lacks, and the reason this interface
  // exists at all: a ReplaySource finishes and a LiveSource does not, and a
  // caller that cannot tell the difference either hangs on a replay or
  // truncates a live feed.
  virtual bool finished() const noexcept = 0;

  // Block until every symbol has reached Live (splice) or the replay has
  // finished. Returns false on timeout. The parity test uses this instead of a
  // sleep; so does any caller reporting readiness.
  virtual bool awaitReady(std::int64_t timeoutMs) = 0;

  // ---- event-time clock -------------------------------------------------
  //
  // THE SOURCE OWNS TIME. Every window boundary in the DAG is crossed because
  // this value advanced, never because a host clock ticked
  // (docs/data-layer.md C1). Under replay it advances at replay rate; under
  // live it tracks the feed.
  //
  // The watermark is the lower bound on future event times: no record with
  // tsEventNs < watermarkNs() will be delivered after this returns. A window
  // closes when the watermark passes its boundary. How far behind the newest
  // record the watermark lags is the one piece of event-time semantics this
  // design does not settle — docs Q10.
  virtual std::int64_t watermarkNs() const noexcept = 0;

  // ---- observation ------------------------------------------------------
  virtual std::vector<SpliceStatus> status() const = 0;
};

// ---------------------------------------------------------------------------
// LiveSource
// ---------------------------------------------------------------------------

// Tail only. No history, no splice, no warm-up. finished() is always false.
//
// This is what today's market.wsclient effectively is, with event time and a
// sequence number added. It exists separately from SplicedSource so a
// deployment with no history configured is a supported configuration rather
// than a degraded one.
class LiveSource final : public ISource {
public:
  struct Config {
    std::vector<std::string> symbols;
    bool  wantL2 = false;
    // Watermark lag: how far behind the newest observed event time the
    // watermark is held, to tolerate late arrivals. docs Q10.
    std::int64_t watermarkLagNs = 0;
  };

  LiveSource(Config cfg,
             std::shared_ptr<IProviderAdapter> adapter,
             std::shared_ptr<INormalizer> normalizer,
             SourceTargets targets);
  ~LiveSource() override;

  std::string_view name() const noexcept override;
  void start() override;
  void stop() noexcept override;
  bool finished() const noexcept override;          // always false
  bool awaitReady(std::int64_t timeoutMs) override;  // first record per symbol
  std::int64_t watermarkNs() const noexcept override;
  std::vector<SpliceStatus> status() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// ReplaySource
// ---------------------------------------------------------------------------

// History only, over a closed window, then finished(). No live subscription.
// For backtests and for the replay half of the parity test.
//
// DETERMINISM CONTRACT (docs/data-layer.md C6). Byte-identical output requires
// all three, and run() ASSERTS the first two rather than documenting them:
//   1. single-threaded ingress — the ENC-1005 precondition Dispatcher.hpp
//      states and does not enforce;
//   2. threadPoolSize == 1;
//   3. ThreadPool::drain() before run() returns — the deterministic barrier
//      ENC-1340 established. A sleep is not a barrier, it is a wider window.
//
// It does NOT promise determinism for a DAG containing a pull node that is not
// adjacent to a clock hop: ENC-1335 binds the sample to the clock only at the
// two adjacency points, and deeper placements keep execution-time sampling by
// design. docs C10 / Q5.
class ReplaySource final : public ISource {
public:
  struct Config {
    // The SAME JSON node graph TreeBuilder consumes. Passed to data::Planner,
    // which derives symbols, columns, record kinds, shards AND THE WARM-UP
    // DEPTH from it (docs §5.1). There is deliberately no `lookbackNs` here:
    // a human setting warm-up per request gets it wrong silently, and in the
    // direction of too little, producing output that looks plausible and is
    // warm-up garbage.
    std::string manifestJson;
    std::vector<std::string> symbols;
    TimeRange   window;                            // half-open [from, to)
    ReplayMode  mode = ReplayMode::Emit;
    // Seek the nearest persisted book snapshot at or before the effective
    // window start and apply it before any delta (docs §9.5, §8.5). Without it
    // an L2 replay starts from an empty book and every early depth read is
    // wrong.
    bool        seedFromSnapshot = true;
    ShimConfig  shim{};
  };

  ReplaySource(Config cfg,
               std::shared_ptr<IL1Store> store,
               SourceTargets targets);
  ~ReplaySource() override;

  std::string_view name() const noexcept override;
  void start() override;                            // async; run() is the sync form
  void stop() noexcept override;
  bool finished() const noexcept override;
  bool awaitReady(std::int64_t timeoutMs) override;
  std::int64_t watermarkNs() const noexcept override;
  std::vector<SpliceStatus> status() const override;

  // Synchronous drive-to-completion. Returns after the final
  // ThreadPool::drain(), so on return every value the window produces has been
  // delivered. This is what the parity test calls.
  void run();

  std::uint64_t rowsReplayed() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// SplicedSource
// ---------------------------------------------------------------------------

struct SpliceConfig {
  std::vector<std::string> symbols;
  bool  wantL2 = false;

  // The manifest, for the same reason ReplaySource::Config carries one: the
  // warm-up depth is DERIVED from the graph by data::Planner, not configured
  // (docs §5.1). A period-200 indicator needs 200 bars of input before the
  // window start and only the graph knows that.
  std::string manifestJson;

  // Default Suppress: a live subscription should not emit its warm-up
  // (docs C4, Q3).
  ReplayMode replayMode = ReplayMode::Suppress;

  // docs §4.3. Overflow during a slow backfill has three options and only one
  // is safe: drop-oldest and drop-newest both punch a silent hole, so overflow
  // ABORTS AND RETRIES the splice with a later T. Each abort increments
  // SpliceStatus::spliceAborts, because a deployment that aborts repeatedly has
  // a backfill too slow for its feed rate and the operator must see that
  // rather than a quietly thin chart.
  std::size_t  maxBufferedRecordsPerSymbol = 0;
  std::uint32_t maxSpliceAttempts = 0;

  // How long to wait for INormalizer::flushedThroughNs to reach the splice
  // point before declaring the window degraded. docs §8.1 — this is what keeps
  // "not written yet" from being read as "no data".
  std::int64_t  flushWaitTimeoutMs = 0;

  std::int64_t  watermarkLagNs = 0;      // docs Q10
  bool          seedFromSnapshot = true;
};

// DEMOTED, DELIBERATELY (docs/data-layer.md §9). Databento Plus is not
// affordable, so there is no live Databento feed, so nothing in the research or
// chart-fill path splices. The only thing exercising this is feed-simulator.
// It stays designed because chart-attach-then-live needs it and live arrives
// when the plan tier does — but it is NOT on the critical path and must not
// lead the ticket order. Named risk: it will ship exercised only against a
// simulator.
//
// Subscribe-and-buffer, backfill, replay, drain, pass through.
//
// PER-SYMBOL STATE, NOT PER-SOURCE (docs §4.2). AAPL may be Live while MSFT is
// still Backfilling, because history returns at different depths and first
// live records arrive at different times. A single source-wide state machine
// stalls every symbol behind the slowest, which on an OTC feed is a symbol
// that may not print for an hour. The source-level state is the minimum over
// symbols.
//
// The five steps, with the two orderings that are load-bearing:
//
//   1. Subscribe live and buffer.                 <-- BEFORE the query, always.
//                                                     Query first and the window
//                                                     between "history ends" and
//                                                     "live begins" is lost with
//                                                     no error anywhere.
//   2. Query history [splicePoint - lookback, splicePoint).
//   3. Replay it through the DAG (mode per config).
//   4. Drain the buffer, dropping (sessionId, providerSeq) <= lastReplayed.
//                                                 <-- the PAIR, never the bare
//                                                     seq: a normalizer restart
//                                                     resets a synthesized seq
//                                                     to 0 (docs §8.4).
//   5. Pass through.
//
// A provider disconnect re-enters the machine at Resync -> Backfilling from
// lastDeliveredEventTimeNs. That is the ordinary splice run again, which is why
// this is a reusable state machine and not a startup script (docs §8.1).
class SplicedSource final : public ISource {
public:
  SplicedSource(SpliceConfig cfg,
                std::shared_ptr<IProviderAdapter> adapter,
                std::shared_ptr<INormalizer> normalizer,
                std::shared_ptr<IL1Store> store,
                SourceTargets targets);
  ~SplicedSource() override;

  std::string_view name() const noexcept override;
  void start() override;
  void stop() noexcept override;
  bool finished() const noexcept override;          // false unless Failed
  bool awaitReady(std::int64_t timeoutMs) override;  // every symbol Live
  std::int64_t watermarkNs() const noexcept override;
  std::vector<SpliceStatus> status() const override;

  // Force a re-splice for one symbol. The data layer installs this as the
  // REPLAY/RECOVERY half of OrderBookManager::requestSnapshotFn, so an L2 gap
  // detected by onSeq() re-snapshots through the same path a disconnect takes
  // (docs §4.6 step 3).
  void resync(std::string_view symbol);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Engine wiring
// ---------------------------------------------------------------------------

// Adapts an ISource to the engine's existing ingress seam, so a source is
// selected by an `ingress.N.kind` INI edit and nothing in main.cpp changes
// (ENC-31). finished() is not expressible through IIngressSource; a finished
// replay simply stops producing, and a caller that needs to know asks the
// ISource.
class SourceIngress final : public engine::IIngressSource {
public:
  explicit SourceIngress(std::shared_ptr<ISource> src);
  ~SourceIngress() override;
  void start() override;
  void stop() noexcept override;
  ISource& source() noexcept;

private:
  std::shared_ptr<ISource> src_;
  bool started_ = false;
};

// Registers "data.live", "data.replay" and "data.spliced" on
// engine::IngressRegistry. Called from the connector's registerWith(), the same
// way market.feedserver / market.wsclient are registered today.
void registerIngressKinds(engine::EngineRegistries& regs);

} // namespace gma::data
