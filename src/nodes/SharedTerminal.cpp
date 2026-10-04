#include "gma/nodes/SharedTerminal.hpp"

#include <algorithm>
#include <exception>

#include "gma/util/Logger.hpp"

namespace gma::nodes {

SharedTerminal::SubscriberId SharedTerminal::attach(std::shared_ptr<INode> sink) {
  if (!sink) return 0;
  std::lock_guard<std::mutex> lk(mx_);
  // After shutdown there is no DAG feeding this node any more, so accepting a
  // sink would register a subscriber that can never receive a value. Refusing
  // with 0 lets the caller fall back to building its own DAG rather than
  // silently joining a dead one.
  if (stopping_.load(std::memory_order_acquire)) return 0;
  const SubscriberId id = nextId_++;
  sinks_.emplace_back(id, std::move(sink));
  return id;
}

SharedTerminal::Detached SharedTerminal::detach(SubscriberId id) {
  Detached out;
  std::lock_guard<std::mutex> lk(mx_);
  auto it = std::find_if(sinks_.begin(), sinks_.end(),
                         [id](const auto& kv) { return kv.first == id; });
  if (it != sinks_.end()) {
    out.sink  = std::move(it->second);
    out.found = true;
    sinks_.erase(it);
  }
  out.remaining = sinks_.size();
  return out;
}

std::size_t SharedTerminal::size() const noexcept {
  std::lock_guard<std::mutex> lk(mx_);
  return sinks_.size();
}

void SharedTerminal::onValue(const StreamValue& sv) {
  // Early-out is an optimization; correctness comes from the snapshot below —
  // if shutdown() races we observe an emptied vector.
  if (stopping_.load(std::memory_order_acquire)) return;

  std::vector<std::pair<SubscriberId, std::shared_ptr<INode>>> snapshot;
  {
    std::lock_guard<std::mutex> lk(mx_);
    snapshot = sinks_;
  }

  for (const auto& kv : snapshot) {
    if (!kv.second) continue;
    // Per-sink catch: see the header. One subscriber throwing must not cost the
    // others this value. `Responder::onValue` already swallows its own callback
    // failures, so reaching this handler means a sink type that does not — and
    // the one thing that must not happen is a silent partial fan-out.
    try {
      kv.second->onValue(sv);
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "shared_terminal.sink_threw",
                              {{"subscriberId", std::to_string(kv.first)},
                               {"streamKey", sv.symbol},
                               {"err", ex.what()}});
    } catch (...) {
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "shared_terminal.sink_threw",
                              {{"subscriberId", std::to_string(kv.first)},
                               {"streamKey", sv.symbol},
                               {"err", "unknown exception"}});
    }
  }
}

void SharedTerminal::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);

  std::vector<std::pair<SubscriberId, std::shared_ptr<INode>>> snapshot;
  {
    std::lock_guard<std::mutex> lk(mx_);
    snapshot.swap(sinks_);
  }

  // Outside the lock. shutdown() is noexcept and idempotent on every node, so
  // this is safe even when a subscriber's own keepAlive list also shuts its
  // Responder down.
  for (auto& kv : snapshot) {
    if (kv.second) kv.second->shutdown();
  }
}

} // namespace gma::nodes
