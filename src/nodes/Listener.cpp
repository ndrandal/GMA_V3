#include "gma/nodes/Listener.hpp"
#include "gma/Dispatcher.hpp"
#include "gma/rt/ThreadPool.hpp"
#include "gma/util/Logger.hpp"

using namespace gma::nodes;

namespace {

// ENC-101: ob.* atomic keys are written by ob::Provider into the
// AtomicStore but never push through Dispatcher::notifyListeners,
// so a Listener bound to one of them registers and silently never
// fires. Reject at construction time so the failure is loud.
//
// Literal 3-char prefix ('o','b','.') — not a starts_with("ob")
// check, which would catch "obesity" or "obvious".
bool isPipelineOnlyKey(const std::string& field) {
  return field.size() >= 3
      && field[0] == 'o'
      && field[1] == 'b'
      && field[2] == '.';
}

} // anonymous namespace

gma::Result<std::shared_ptr<Listener>> Listener::Create(
    std::string symbol,
    std::string field,
    std::shared_ptr<INode> downstream,
    gma::rt::ThreadPool* pool,
    gma::Dispatcher* dispatcher,
    std::shared_ptr<gma::rt::Strand> strand) {
  if (isPipelineOnlyKey(field)) {
    return gma::Error{
      "listener: field '" + field +
        "' is pipeline-only — see docs/atomic-keys.md; bind via "
        "AtomicAccessor downstream of a bare-key Listener clock",
      "Listener::Create"
    };
  }
  auto self = std::make_shared<Listener>(
      std::move(symbol),
      std::move(field),
      std::move(downstream),
      pool,
      dispatcher,
      std::move(strand));
  self->start();
  return self;
}

Listener::Listener(std::string symbol,
                   std::string field,
                   std::shared_ptr<INode> downstream,
                   gma::rt::ThreadPool* pool,
                   gma::Dispatcher* dispatcher,
                   std::shared_ptr<gma::rt::Strand> strand)
  : symbol_(std::move(symbol))
  , field_(std::move(field))
  , downstream_(std::move(downstream))
  , pool_(pool)
  , dispatcher_(dispatcher)
  , strand_(std::move(strand))
{
}

void Listener::start() {
  // Must be called after construction when owned by a shared_ptr.
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return; // already started

  if (dispatcher_) {
    dispatcher_->registerListener(symbol_, field_, shared_from_this());
  }
}

void Listener::onValue(const gma::StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::shared_ptr<INode> down;
  {
    std::lock_guard<std::mutex> lk(downMx_);
    down = downstream_.lock();
  }
  if (!down) return;

  // ENC-1005 / SPEC specs/2026-09-20-gma-join-correctness D3, D4.
  //
  // The STRAND is the whole fix. Every Listener in one request DAG shares one,
  // so the values this DAG is built to join arrive downstream in the order the
  // Dispatcher produced them, tick after tick. Posting to the bare pool (the
  // `else if` below) is what put tick N and tick N+1 — and `ask` and `bid` of
  // one tick — on two workers at once, which is how `Aggregate`'s buffer came
  // to pair `ask(n)` with `ask(n+1)` and report a spread of +-1.00 where the
  // truth is 0.02 (SPEC §1.1 defect 4).
  //
  // A strand is not a thread and adds none: the work still runs on the shared
  // pool, one task at a time per DAG. The cost is that one DAG can no longer
  // occupy more than one core, which SPEC §5 Q2 rules acceptable.
  //
  // The strand path forwards the whole `StreamValue`, so `bucketStartMs`
  // (ENC-1280) survives; the pool path below rebuilds a two-field value and
  // drops it. That is not a behaviour change today — `Dispatcher` constructs
  // every pushed value with `bucketStartMs == 0` (StreamValue.hpp:134-138
  // calls the raw Listener path "no bucket identity"), so there is nothing to
  // lose on either path. It is written the correct way here so the difference
  // does not become one later.
  if (strand_) {
    strand_->post([d = down, sv]() mutable {
      d->onValue(sv);
    });
  } else if (pool_) {
    pool_->post([d = std::move(down), sym = sv.symbol, val = sv.value]() mutable {
      d->onValue(gma::StreamValue{std::move(sym), std::move(val)});
    });
  } else {
    down->onValue(sv);
  }
}

void Listener::shutdown() noexcept {
  bool expected = false;
  if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return; // already shutting down

  try {
    if (dispatcher_) {
      dispatcher_->unregisterListener(symbol_, field_, shared_from_this());
    }
  } catch (const std::exception& ex) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "Listener shutdown error",
                            { {"symbol", symbol_}, {"err", ex.what()} });
  } catch (...) {
    gma::util::logger().log(gma::util::LogLevel::Error,
                            "Listener shutdown unknown error",
                            { {"symbol", symbol_} });
  }
  {
    std::lock_guard<std::mutex> lk(downMx_);
    downstream_.reset();
  }
}
