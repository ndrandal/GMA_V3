#include "gma/nodes/Interval.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

Interval::Interval(std::chrono::milliseconds period,
                   std::shared_ptr<INode> child,
                   gma::rt::ThreadPool* pool)
  : state_(std::make_shared<State>(period, std::move(child), pool))
{
}

Interval::~Interval() {
  // The timer thread holds no reference back to this object (ENC-1065), so
  // reaching the destructor at all is the normal path even when nobody called
  // shutdown(). Stop and join here so no thread outlives the Interval.
  Interval::shutdown();
}

void Interval::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return; // already started

  // Capture the State, never `this` and never a shared_from_this().
  timerThread_ = std::thread([st = state_] { timerLoop(st); });
}

void Interval::timerLoop(const std::shared_ptr<State>& st) {
  while (true) {
    {
      std::unique_lock<std::mutex> lk(st->mx);
      if (st->cv.wait_for(lk, st->period, [&st] {
            return st->stopping.load(std::memory_order_acquire);
          })) {
        break; // woken by shutdown
      }
    }

    if (st->stopping.load(std::memory_order_acquire))
      break;

    if (!st->child) break; // child gone, stop ticking

    try {
      if (st->pool) {
        auto c = st->child;
        st->pool->post([c] { c->onValue(StreamValue{"", 0.0}); });
      } else {
        st->child->onValue(StreamValue{"", 0.0});
      }
    } catch (const std::exception& ex) {
      gma::util::logger().log(gma::util::LogLevel::Error,
        "Interval::timerLoop: onValue exception",
        {{"err", ex.what()}});
    }
  }
}

void Interval::onValue(const StreamValue&) {
  // source node: no upstream input
}

void Interval::shutdown() noexcept {
  state_->stopping.store(true, std::memory_order_release);
  state_->cv.notify_all();
  if (timerThread_.joinable()) {
    // If shutdown() is called from the timer thread itself (e.g. via a
    // downstream callback), join() would deadlock. Detach instead: the thread
    // owns the State it is still reading, so it can finish safely even if the
    // Interval is destroyed first.
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
}

} // namespace gma
