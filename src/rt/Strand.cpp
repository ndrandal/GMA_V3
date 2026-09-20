#include "gma/rt/Strand.hpp"

#include "gma/util/Logger.hpp"

namespace gma::rt {

void Strand::post(std::function<void()> fn) {
  if (!fn) return;

  bool kick = false;
  {
    std::lock_guard<std::mutex> lk(mx_);
    q_.push_back(std::move(fn));
    if (!running_) {
      running_ = true;
      kick     = true;
    }
  }
  if (!kick) return;                 // an existing drainer will pick it up

  if (!pool_) {
    // No executor: run inline, still in order. This is the same fallback
    // `Listener::onValue` already had when `pool_` was null, and it keeps the
    // ordering guarantee — it just borrows the caller's thread for it.
    drain();
    return;
  }

  // `shared_from_this` keeps the strand alive for the whole drain, so the last
  // external owner may drop while work is still queued.
  if (pool_->post([self = shared_from_this()] { self->drain(); }))
    return;

  // THE KICK WAS REFUSED — the pool is stopping and silently drops posts
  // (`ThreadPool::post` returns false). We hold the drainer token, and nothing
  // else will ever clear it, so leaving now wedges this strand permanently and
  // silently: every later post appends to a queue no drainer will visit, and
  // the queued closures pin the whole DAG alive. Found by the ENC-1005
  // adversarial review.
  //
  // So drain inline instead. The caller's thread is the only executor left, and
  // the ordering guarantee is kept. Logged once per occurrence because running
  // DAG compute on an ingress or shutdown thread is a fact an operator should
  // be able to see, not a silent fallback.
  gma::util::logger().log(gma::util::LogLevel::Warn,
    "Strand: thread pool refused the drain kick (stopping) — draining inline",
    {{"queued", std::to_string(queueDepth())}});
  drain();
}

void Strand::drain() {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lk(mx_);
      if (q_.empty()) {
        // Clearing `running_` under the same lock that `post` takes is what
        // makes the handoff race-free: a post that appends after this point
        // sees `running_ == false` and kicks a fresh drainer, and one that
        // appends before it is still in `q_` and taken by this loop.
        running_ = false;
        return;
      }
      fn = std::move(q_.front());
      q_.pop_front();
    }
    // ENC-1338: count the completion, whatever the task did. Bumped after the
    // call so an in-flight task is not yet counted, and under the same mutex
    // `tasksRun()` reads, so the counter needs no atomic of its own.
    struct Counted {
      Strand* s;
      ~Counted() {
        std::lock_guard<std::mutex> lk(s->mx_);
        ++s->tasksRun_;
      }
    } counted{this};
    // Outside the lock: a task may post back onto this same strand.
    try {
      fn();
    } catch (const std::exception& e) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "Strand: task exception", {{"err", e.what()}});
    } catch (...) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "Strand: unknown task exception");
    }
  }
}

std::size_t Strand::queueDepth() const {
  std::lock_guard<std::mutex> lk(mx_);
  return q_.size();
}

} // namespace gma::rt
