#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "gma/Result.hpp"
#include "gma/nodes/INode.hpp"
#include "gma/rt/Strand.hpp"
#include "gma/rt/ThreadPool.hpp"

namespace gma {

class Dispatcher;

namespace nodes {

class Listener final
  : public INode
  , public std::enable_shared_from_this<Listener>
{
public:
  // Prefer Create() over the public constructor: it enforces the
  // ENC-101 push-vs-pull rule (see GMA_V3/docs/atomic-keys.md) by
  // rejecting `ob.*` fields, which the dispatcher never notifies on
  // and which would silently produce zero updates. The constructor
  // remains public so unit tests can build a Listener with arbitrary
  // fields to exercise the reject path itself.
  //
  // On success the returned Listener has already had `start()`
  // called; callers do not need the two-step pattern. On failure the
  // returned Result carries an Error whose `message` field begins
  // with `"listener: field '<field>' is pipeline-only"` and points
  // at `docs/atomic-keys.md`.
  // `strand` (ENC-1005 / SPEC D3) is this request DAG's serializing executor.
  // When present every value is delivered through it, so the whole DAG sees
  // values in the order the Dispatcher produced them. When null the Listener
  // keeps the pre-ENC-1005 behaviour exactly: a bare `pool->post`, unordered.
  static gma::Result<std::shared_ptr<Listener>> Create(
      std::string symbol,
      std::string field,
      std::shared_ptr<INode> downstream,
      gma::rt::ThreadPool* pool,
      gma::Dispatcher* dispatcher,
      std::shared_ptr<gma::rt::Strand> strand = nullptr);

  Listener(std::string symbol,
           std::string field,
           std::shared_ptr<INode> downstream,
           gma::rt::ThreadPool* pool,
           gma::Dispatcher* dispatcher,
           std::shared_ptr<gma::rt::Strand> strand = nullptr);

  // IMPORTANT:
  // Do NOT register with Dispatcher from the constructor.
  // shared_from_this() is not valid until the object is owned by a shared_ptr.
  // Call start() immediately after construction (or use Create()).
  void start();

  // INode
  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  // ENC-1005 / SPEC D3. True exactly when this Listener holds a strand: the
  // strand does the off-threading, so Dispatcher must deliver inline rather
  // than interposing its own pool hop and losing the order. See INode.
  bool deliversOnOwnExecutor() const noexcept override {
    return false;  // MUTATION M1
  }

  const std::string& symbol() const noexcept { return symbol_; }
  const std::string& field()  const noexcept { return field_;  }

private:
  std::string symbol_;
  std::string field_;

  mutable std::mutex downMx_;
  std::weak_ptr<INode> downstream_;
  gma::rt::ThreadPool* pool_;          // canonical type
  gma::Dispatcher* dispatcher_;
  // This request DAG's serializing executor (ENC-1005). Shared by every
  // Listener in the same DAG — that sharing is what orders the two sides of a
  // join against each other, and what a per-SYMBOL shard could never do
  // (SPEC D4).
  std::shared_ptr<gma::rt::Strand> strand_;

  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
};

} // namespace nodes
} // namespace gma
