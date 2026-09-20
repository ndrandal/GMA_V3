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
  // `pool` must outlive every task posted to this strand. In production that is
  // ExecutionContext's pool, which outlives all sessions.
  explicit Strand(ThreadPool* pool) : pool_(pool) {}

  Strand(const Strand&)            = delete;
  Strand& operator=(const Strand&) = delete;
  Strand(Strand&&)                 = delete;
  Strand& operator=(Strand&&)      = delete;

  // Enqueue `fn`. It runs after every task already posted to this strand and
  // before every task posted after it. Never runs inline on the caller.
  void post(std::function<void()> fn);

  // Diagnostics only — never a synchronisation point.
  std::size_t queueDepth() const;

private:
  void drain();

  ThreadPool* pool_;
  mutable std::mutex                mx_;
  std::deque<std::function<void()>> q_;
  // True while a drain task is live on the pool. It is the token that makes
  // this a strand: exactly one drainer at a time, so two tasks of the same
  // strand can never be in flight together.
  bool running_{false};
};

} // namespace gma::rt
