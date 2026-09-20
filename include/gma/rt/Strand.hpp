// File: include/gma/rt/Strand.hpp
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "gma/rt/ThreadPool.hpp"

namespace gma::rt {

// ENC-1005 / SPEC specs/2026-09-20-gma-join-correctness D3, D4.
//
// A SERIALIZING EXECUTOR over `ThreadPool`. Tasks posted to one Strand run
//   * one at a time, and
//   * in the order they were posted,
// while still running on the shared pool's worker threads. It is the classic
// "strand over an executor" construction — the same guarantee `asio::strand`
// gives, expressed over the executor GMA actually has.
//
// WHY NOT `asio::strand`, which D3 names. GMA's node graph does not run on an
// `asio::io_context`: it runs on `gma::rt::ThreadPool` (`src/rt/ThreadPool.cpp`),
// a hand-rolled mutex+condvar queue with no asio executor behind it. The only
// `io_context` in the process belongs to `WebSocketServer`/`ClientSession` and
// drives sockets. Minting a real `asio::strand` would therefore have meant
// moving every node's compute onto the socket I/O threads — a far larger change
// than D3 asks for, and one that puts unbounded user compute in front of the
// read loop. This class is the same abstraction bound to the right executor;
// the deviation is in the mechanism, not in the guarantee.
//
// THREAD COUNT DOES NOT CHANGE. A Strand owns no thread. It occupies one pool
// worker only while it has work, and N strands still occupy up to N cores —
// which is why SPEC §5 Q2 rules the single-subscription regression acceptable.
//
// LIFETIME. A running strand holds a `shared_ptr` to itself for the duration of
// its drain task, so a Strand whose last external owner drops while work is
// queued still finishes that work rather than freeing itself underneath a pool
// thread.
class Strand : public std::enable_shared_from_this<Strand> {
public:
  // ENC-1338 — WHICH SITE MINTED THIS STRAND.
  //
  // D3 names ONE mint site: `ClientSession::handleSubscribe`, one strand per
  // subscription. `tree::buildForRequest` mints a second one as a backstop so
  // ordering is a property of the DAG rather than of the caller's discipline
  // (see the comment there). The two are behaviourally indistinguishable from
  // outside — which is exactly why ENC-1005's mutations M7 (delete the
  // `handleSubscribe` mint) and M10 (unguard it) reddened NOTHING: the backstop
  // silently supplied a replacement and every assertion in the suite stayed
  // true. The production guarantee rested on a line that could be deleted with
  // a green suite.
  //
  // This tag is the missing observable. It is a `const char*` literal, set once
  // at construction and never read on any hot path, that says which site minted
  // this strand. `tests/ws/SubscriptionStrandMintTest.cpp` reads it off the
  // head `Listener` of a subscription driven through a REAL WebSocket session
  // and asserts it is the session site — so a DAG that silently fell back to
  // the backstop names itself.
  //
  // It is not test-only scaffolding: with two mint sites and a granularity
  // argument attached to each, "which executor is this DAG on, and who gave it
  // one" is a question a log line should be able to answer.
  //
  // DEFAULT IS DELIBERATELY NOT "buildForRequest". Anything that did not go
  // through a site that names itself reads `unattributed`, which today means
  // the backstop or a test-constructed strand. A mint inlined at a call site —
  // the shape M10 takes when it is applied to the call rather than to the
  // helper — therefore also reads `unattributed` and is caught by the same
  // assertion.
  static constexpr const char* kUnattributedOrigin = "unattributed";

  // MUST BE OWNED BY A `shared_ptr` — `std::make_shared<Strand>(pool)`.
  // `post()` calls `shared_from_this()`, so a stack-allocated Strand throws
  // `std::bad_weak_ptr` on its first post. Same contract `nodes::Listener` has
  // for `start()`, and for the same reason. Every construction site in the
  // engine goes through `tree::Deps::strand`, which is a `shared_ptr` by type.
  //
  // `pool` must outlive every task posted to this strand. In production that is
  // ExecutionContext's pool, which outlives all sessions.
  explicit Strand(ThreadPool* pool,
                  const char* origin = kUnattributedOrigin)
    : pool_(pool)
    , origin_(origin ? origin : kUnattributedOrigin) {}

  Strand(const Strand&)            = delete;
  Strand& operator=(const Strand&) = delete;
  Strand(Strand&&)                 = delete;
  Strand& operator=(Strand&&)      = delete;

  // Enqueue `fn`. It runs after every task already posted to this strand and
  // before every task posted after it.
  //
  // WHERE IT RUNS. Normally on a pool worker, never on the caller. There are
  // exactly two exceptions, both of which run the queue INLINE ON THE CALLER
  // rather than dropping it, and both of which still run it in order:
  //   * the strand has no pool at all (`Strand(nullptr)`), and
  //   * `ThreadPool::post` refused the kick because the pool is stopping.
  // The second is the one that matters: `running_` is set before the kick, so a
  // silently dropped kick used to leave the token set forever and the strand
  // permanently, silently wedged — every later post appended to a queue that no
  // drainer would ever visit, with the DAG pinned alive by the queued closures.
  // Found by an adversarial review of ENC-1005; gated by
  // `StrandDeliversEvenWhenThePoolIsAlreadyStopping`.
  void post(std::function<void()> fn);

  // Diagnostics only — never a synchronisation point.
  std::size_t queueDepth() const;

  // ENC-1338. The mint site that created this strand; see `kUnattributedOrigin`
  // above. Never null — the constructor substitutes the default for a null
  // argument. Compare with `==`/`strcmp` against the site's own accessor
  // (e.g. `gma::server::subscriptionStrandOrigin()`), never against a literal
  // spelled out a second time.
  const char* origin() const noexcept { return origin_; }

private:
  void drain();

  ThreadPool* pool_;
  const char* origin_;
  mutable std::mutex                mx_;
  std::deque<std::function<void()>> q_;
  // True while a drain task is live on the pool. It is the token that makes
  // this a strand: exactly one drainer at a time, so two tasks of the same
  // strand can never be in flight together.
  bool running_{false};
};

} // namespace gma::rt
