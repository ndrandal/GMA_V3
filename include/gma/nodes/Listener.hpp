#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "gma/Result.hpp"
#include "gma/nodes/INode.hpp"
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
  static gma::Result<std::shared_ptr<Listener>> Create(
      std::string symbol,
      std::string field,
      std::shared_ptr<INode> downstream,
      gma::rt::ThreadPool* pool,
      gma::Dispatcher* dispatcher);

  Listener(std::string symbol,
           std::string field,
           std::shared_ptr<INode> downstream,
           gma::rt::ThreadPool* pool,
           gma::Dispatcher* dispatcher);

  // IMPORTANT:
  // Do NOT register with Dispatcher from the constructor.
  // shared_from_this() is not valid until the object is owned by a shared_ptr.
  // Call start() immediately after construction (or use Create()).
  void start();

  // INode
  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

  const std::string& symbol() const noexcept { return symbol_; }
  const std::string& field()  const noexcept { return field_;  }

private:
  std::string symbol_;
  std::string field_;

  mutable std::mutex downMx_;
  std::weak_ptr<INode> downstream_;
  gma::rt::ThreadPool* pool_;          // canonical type
  gma::Dispatcher* dispatcher_;

  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
};

} // namespace nodes
} // namespace gma
