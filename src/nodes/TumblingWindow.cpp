#include "gma/nodes/TumblingWindow.hpp"
#include "gma/nodes/BucketTime.hpp"   // for BucketTime::nextAlignedAfter
#include "gma/util/Logger.hpp"

#include <utility>
#include <variant>

namespace gma {

namespace {

// File-local mirror of TreeBuilder.cpp's toDouble — that one lives in an
// anonymous namespace and isn't exported. Keep the variant cases identical
// (bool/int/double mapped numerically; vector cases drop to 0 since a
// TumblingWindow is fed scalar streams).
double toDouble(const ArgType& v) {
  return std::visit(
    [](auto&& x) -> double {
      using T = std::decay_t<decltype(x)>;
      if constexpr (std::is_same_v<T, bool>)        return x ? 1.0 : 0.0;
      else if constexpr (std::is_same_v<T, int>)    return static_cast<double>(x);
      else if constexpr (std::is_same_v<T, double>) return x;
      else                                          return 0.0;
    },
    v
  );
}

} // namespace

TumblingWindow::TumblingWindow(std::chrono::milliseconds period,
                               std::shared_ptr<INode> downstream,
                               gma::rt::ThreadPool* pool)
  : state_(std::make_shared<State>(period, std::move(downstream), pool))
{
}

TumblingWindow::~TumblingWindow() {
  // The timer thread holds no reference back to this object (ENC-1080), so
  // reaching the destructor at all is the normal path even when nobody called
  // shutdown(). Stop and join here so no thread outlives the TumblingWindow.
  TumblingWindow::shutdown();
}

void TumblingWindow::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return;

  // Capture the State, never `this` and never a shared_from_this().
  timerThread_ = std::thread([st = state_] { timerLoop(st); });
}

void TumblingWindow::onValue(const StreamValue& sv) {
  if (state_->stopping.load(std::memory_order_acquire)) return;

  std::lock_guard<std::mutex> lk(state_->mx);
  // Refuse unbounded growth from pathological inputs (mirrors Worker's
  // MAX_SYMBOLS guard at Worker.cpp:20-25).
  if (state_->acc.find(sv.symbol) == state_->acc.end() &&
      state_->acc.size() >= MAX_SYMBOLS) {
    gma::util::logger().log(gma::util::LogLevel::Warn,
      "TumblingWindow: max symbols reached, dropping",
      {{"symbol", sv.symbol}});
    return;
  }
  state_->acc[sv.symbol].push_back(toDouble(sv.value));
}

void TumblingWindow::timerLoop(const std::shared_ptr<State>& st) {
  while (true) {
    if (st->stopping.load(std::memory_order_acquire))
      break;

    const auto now = std::chrono::system_clock::now();
    const auto target = BucketTime::nextAlignedAfter(now, st->period);
    {
      std::unique_lock<std::mutex> lk(st->mx);
      // wait_until lets shutdown wake us early without re-arming. The
      // predicate ensures we don't wake spuriously and emit early.
      if (st->cv.wait_until(lk, target, [&st] {
            return st->stopping.load(std::memory_order_acquire);
          })) {
        break;
      }
    }
    if (st->stopping.load(std::memory_order_acquire))
      break;

    // Snapshot the downstream AND the non-empty buckets under the lock — move
    // out the per-symbol vectors into a local list. `swap()` on the moved-from
    // vector keeps its capacity for the next bucket (steady-state
    // alloc-bounded). We release the lock before calling downstream so a
    // re-entrant downstream (e.g. routed back into another TumblingWindow)
    // can't deadlock. `downstream` is read here rather than outside the lock
    // because shutdown() may reset it concurrently.
    std::shared_ptr<INode> ds;
    std::vector<std::pair<std::string, std::vector<double>>> emits;
    {
      std::lock_guard<std::mutex> lk(st->mx);
      ds = st->downstream;
      if (!ds) break;
      emits.reserve(st->acc.size());
      for (auto& kv : st->acc) {
        if (kv.second.empty()) continue; // empty bucket: no emit
        std::vector<double> out;
        out.swap(kv.second);             // move-out, leave kv.second empty + capacity-preserved
        emits.emplace_back(kv.first, std::move(out));
      }
    }
    if (emits.empty()) continue;

    for (auto& [sym, vec] : emits) {
      try {
        if (st->pool) {
          // Wrap in a shared_ptr so the captured lambda can hold the vector
          // by value without copying — same trick BucketTime uses for the
          // tick payload. Move into the lambda capture to skip a copy.
          auto sym_cap = sym;
          auto vec_cap = std::make_shared<std::vector<double>>(std::move(vec));
          st->pool->post([ds, s = std::move(sym_cap), v = std::move(vec_cap)] {
            ds->onValue(StreamValue{s, ArgType{*v}});
          });
        } else {
          ds->onValue(StreamValue{sym, ArgType{std::move(vec)}});
        }
      } catch (const std::exception& ex) {
        gma::util::logger().log(gma::util::LogLevel::Error,
          "TumblingWindow::timerLoop: onValue exception",
          {{"symbol", sym}, {"err", ex.what()}});
      }
    }
  }
}

void TumblingWindow::shutdown() noexcept {
  state_->stopping.store(true, std::memory_order_release);
  state_->cv.notify_all();
  if (timerThread_.joinable()) {
    // If shutdown() is called from the timer thread itself (e.g. via a
    // downstream callback), join() would deadlock. Detach instead: the thread
    // owns the State it is still reading, so it can finish safely even if the
    // TumblingWindow is destroyed first.
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
  // Drop downstream + buffers under the lock so any in-flight onValue racing
  // with shutdown sees the stopping flag (and would early-return), and the
  // memory is released promptly.
  std::lock_guard<std::mutex> lk(state_->mx);
  state_->downstream.reset();
  state_->acc.clear();
}

} // namespace gma
