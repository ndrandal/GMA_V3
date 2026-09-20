// include/gma/nodes/INode.hpp
#pragma once
#include "gma/StreamValue.hpp"
#include "gma/AtomicStore.hpp"

namespace gma {

class INode {
public:
  INode() = default;
  virtual ~INode() = default;

  INode(const INode&) = delete;
  INode& operator=(const INode&) = delete;

  virtual void onValue(const StreamValue& sv) = 0;
  virtual void shutdown() noexcept = 0;

  // ENC-1005 / SPEC specs/2026-09-20-gma-join-correctness D3.
  //
  // "I re-post every value onto my own SERIALIZING executor, so do not
  // interpose a queue of your own in front of me."
  //
  // `Dispatcher` normally hands each notification to `ThreadPool::post`, which
  // is where per-DAG ordering is lost: the two sides of one tick (`ask`, then
  // `bid`) become two independent pool tasks and can reach the join in either
  // order. A node that answers true here is called INLINE on the ingress
  // thread instead, and is responsible for getting the work off that thread
  // itself — which a `rt::Strand` does, in post order.
  //
  // Only `nodes::Listener` overrides this, and only when it actually holds a
  // strand. Every other node — and every `Listener` on the legacy unordered
  // path — keeps the pool hop it has today, so this is inert for everything
  // that has not opted in.
  virtual bool deliversOnOwnExecutor() const noexcept { return false; }
};

} // namespace gma
