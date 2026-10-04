#include "gma/server/SharedSubscriptionRegistry.hpp"

#include <utility>

namespace gma::server {

// ------------------------------
// Lease
// ------------------------------
SharedSubscriptionRegistry::Lease::Lease(Lease&& o) noexcept
  : reg_(o.reg_), key_(std::move(o.key_)), id_(o.id_) {
  o.reg_ = nullptr;
  o.id_  = 0;
}

SharedSubscriptionRegistry::Lease&
SharedSubscriptionRegistry::Lease::operator=(Lease&& o) noexcept {
  if (this != &o) {
    release();
    reg_   = o.reg_;
    key_   = std::move(o.key_);
    id_    = o.id_;
    o.reg_ = nullptr;
    o.id_  = 0;
  }
  return *this;
}

SharedSubscriptionRegistry::Lease::~Lease() { release(); }

void SharedSubscriptionRegistry::Lease::release() noexcept {
  if (!reg_) return;
  SharedSubscriptionRegistry* reg = reg_;
  reg_ = nullptr;                 // before the call: release() is idempotent
  reg->release(key_, id_);
  id_ = 0;
}

// ------------------------------
// Registry
// ------------------------------
SharedSubscriptionRegistry::~SharedSubscriptionRegistry() { shutdownAll(); }

SharedSubscriptionRegistry::Lease
SharedSubscriptionRegistry::acquire(const std::string& key,
                                    std::shared_ptr<INode> sink,
                                    const Builder& build) {
  if (!sink || !build) return Lease{};

  std::unique_lock<std::mutex> lk(mu_);

  for (;;) {
    auto it = entries_.find(key);

    if (it == entries_.end()) {
      // We own the build for this key. Publish the Building entry first so a
      // concurrent acquire waits rather than building a second DAG, then drop
      // the lock — `build` may be expensive and must not block unrelated
      // subscribes (the pre-dedup code built outside `ClientSession::reqMu_`
      // for exactly this reason).
      auto entry      = std::make_shared<Entry>();
      entry->terminal = std::make_shared<nodes::SharedTerminal>();
      entries_.emplace(key, entry);

      auto terminal = entry->terminal;
      lk.unlock();

      Built built;
      try {
        built = build(terminal);
      } catch (...) {
        // Do not cache a failure. Erase and wake the waiters so each of them
        // re-runs the build under its OWN request key and is rejected with its
        // own `sendError(..., key)` — ENC-1396's attribution, preserved through
        // the shared path.
        lk.lock();
        auto cur = entries_.find(key);
        if (cur != entries_.end() && cur->second == entry) entries_.erase(cur);
        lk.unlock();
        cv_.notify_all();
        throw;
      }

      lk.lock();
      // `shutdownAll` may have run while we were building. If our entry is no
      // longer the one in the map (or is gone), the DAG we just built has no
      // owner — tear it down rather than attaching a subscriber to it.
      auto cur = entries_.find(key);
      if (cur == entries_.end() || cur->second != entry) {
        lk.unlock();
        cv_.notify_all();
        terminal->shutdown();
        if (built.head) built.head->shutdown();
        for (auto& n : built.keepAlive) {
          if (n) n->shutdown();
        }
        return Lease{};
      }

      entry->head      = std::move(built.head);
      entry->keepAlive = std::move(built.keepAlive);
      entry->building  = false;
      ++builds_;
      cv_.notify_all();

      const auto id = entry->terminal->attach(std::move(sink));
      if (id == 0) return Lease{};
      return Lease{this, key, id};
    }

    auto entry = it->second;
    if (entry->building) {
      // Someone else is building this exact DAG. Wait rather than duplicate it.
      cv_.wait(lk);
      continue;   // re-resolve: the entry may now be Live, or gone (build threw)
    }

    // Live: join it.
    const auto id = entry->terminal->attach(std::move(sink));
    if (id == 0) {
      // The terminal was shut down concurrently; the entry is on its way out.
      return Lease{};
    }
    ++shares_;
    return Lease{this, key, id};
  }
}

void SharedSubscriptionRegistry::release(
    const std::string& key, nodes::SharedTerminal::SubscriberId id) noexcept {
  std::shared_ptr<nodes::SharedTerminal> terminal;
  std::shared_ptr<INode>                 head;
  std::vector<std::shared_ptr<INode>>    keepAlive;
  std::shared_ptr<INode>                 sink;

  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    auto& entry = it->second;
    if (!entry->terminal) return;

    // Removal and the remaining count are read under ONE terminal lock, so a
    // concurrent `attach` cannot slip in between "I was the last" and the
    // teardown below: either it is counted here (remaining > 0, no teardown) or
    // it happens after we have already erased the entry, in which case it
    // attaches to a terminal nobody can reach and `attach` is about to be
    // refused by `stopping_`.
    auto det = entry->terminal->detach(id);
    sink     = std::move(det.sink);
    if (det.remaining == 0) {
      terminal  = std::move(entry->terminal);
      head      = std::move(entry->head);
      keepAlive = std::move(entry->keepAlive);
      entries_.erase(it);
    }
  }

  // Everything below runs with NO registry lock held: `shutdown()` on a timer
  // node joins a thread, and holding `mu_` across that would block every
  // subscribe on the server.
  if (sink) sink->shutdown();
  if (terminal) terminal->shutdown();
  if (head) head->shutdown();
  for (auto& n : keepAlive) {
    if (n) n->shutdown();
  }
}

std::size_t SharedSubscriptionRegistry::liveKeys() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::size_t n = 0;
  for (const auto& kv : entries_) {
    if (!kv.second->building) ++n;
  }
  return n;
}

std::size_t SharedSubscriptionRegistry::refCount(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second->building || !it->second->terminal) return 0;
  return it->second->terminal->size();
}

std::size_t SharedSubscriptionRegistry::buildCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return builds_;
}

std::size_t SharedSubscriptionRegistry::shareCount() const {
  std::lock_guard<std::mutex> lk(mu_);
  return shares_;
}

void SharedSubscriptionRegistry::shutdownAll() noexcept {
  std::unordered_map<std::string, std::shared_ptr<Entry>> taken;
  {
    std::lock_guard<std::mutex> lk(mu_);
    taken.swap(entries_);
  }
  // Wake anything waiting on a Building entry we just dropped.
  cv_.notify_all();

  for (auto& kv : taken) {
    auto& entry = kv.second;
    if (entry->terminal) entry->terminal->shutdown();
    if (entry->head) entry->head->shutdown();
    for (auto& n : entry->keepAlive) {
      if (n) n->shutdown();
    }
  }
}

} // namespace gma::server
