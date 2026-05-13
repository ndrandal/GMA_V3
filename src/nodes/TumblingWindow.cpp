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
  : period_(period), downstream_(std::move(downstream)), pool_(pool)
{
}

TumblingWindow::~TumblingWindow() {
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) {
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
}

void TumblingWindow::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true))
    return;

  timerThread_ = std::thread([self = shared_from_this()] {
    self->timerLoop();
  });
}

void TumblingWindow::onValue(const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  std::lock_guard<std::mutex> lk(mx_);
  // Refuse unbounded growth from pathological inputs (mirrors Worker's
  // MAX_SYMBOLS guard at Worker.cpp:20-25).
  if (acc_.find(sv.symbol) == acc_.end() && acc_.size() >= MAX_SYMBOLS) {
    gma::util::logger().log(gma::util::LogLevel::Warn,
      "TumblingWindow: max symbols reached, dropping",
      {{"symbol", sv.symbol}});
    return;
  }
  acc_[sv.symbol].push_back(toDouble(sv.value));
}

void TumblingWindow::timerLoop() {
  while (true) {
    if (stopping_.load(std::memory_order_acquire))
      break;

    const auto now = std::chrono::system_clock::now();
    const auto target = BucketTime::nextAlignedAfter(now, period_);
    {
      std::unique_lock<std::mutex> lk(mx_);
      // wait_until lets shutdown wake us early without re-arming. The
      // predicate ensures we don't wake spuriously and emit early.
      if (cv_.wait_until(lk, target, [this] {
            return stopping_.load(std::memory_order_acquire);
          })) {
        break;
      }
    }
    if (stopping_.load(std::memory_order_acquire))
      break;
    if (!downstream_) break;

    // Snapshot non-empty buckets under the lock — move out the per-symbol
    // vectors into a local list. `clear()` on the moved-from vector keeps
    // its capacity for the next bucket (steady-state alloc-bounded). We
    // release the lock before calling downstream so a re-entrant downstream
    // (e.g. routed back into another TumblingWindow) can't deadlock.
    std::vector<std::pair<std::string, std::vector<double>>> emits;
    {
      std::lock_guard<std::mutex> lk(mx_);
      emits.reserve(acc_.size());
      for (auto& kv : acc_) {
        if (kv.second.empty()) continue; // empty bucket: no emit
        std::vector<double> out;
        out.swap(kv.second);             // move-out, leave kv.second empty + capacity-preserved
        emits.emplace_back(kv.first, std::move(out));
      }
    }
    if (emits.empty()) continue;

    auto ds = downstream_;
    for (auto& [sym, vec] : emits) {
      try {
        if (pool_) {
          // Wrap in a shared_ptr so the captured lambda can hold the vector
          // by value without copying — same trick BucketTime uses for the
          // tick payload. Move into the lambda capture to skip a copy.
          auto sym_cap = sym;
          auto vec_cap = std::make_shared<std::vector<double>>(std::move(vec));
          pool_->post([ds, s = std::move(sym_cap), v = std::move(vec_cap)] {
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
  stopping_.store(true, std::memory_order_release);
  cv_.notify_all();
  if (timerThread_.joinable()) {
    if (timerThread_.get_id() == std::this_thread::get_id()) {
      timerThread_.detach();
    } else {
      timerThread_.join();
    }
  }
  // Drop downstream + buffers under the lock so any in-flight onValue racing
  // with shutdown sees the stopping_ flag (and would early-return), and the
  // memory is released promptly.
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
  acc_.clear();
}

} // namespace gma
