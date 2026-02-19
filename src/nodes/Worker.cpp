#include "gma/nodes/Worker.hpp"
#include "gma/util/Logger.hpp"

namespace gma {

Worker::Worker(Fn fn, std::shared_ptr<INode> downstream)
  : fn_(std::move(fn)), downstream_(std::move(downstream)) {}

void Worker::onValue(const SymbolValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;

  ArgType out;
  std::shared_ptr<INode> ds;
  {
    std::lock_guard<std::mutex> lk(mx_);
    // Cap distinct symbol count to prevent unbounded map growth from
    // pathological inputs (e.g. millions of unique symbol strings).
    if (acc_.find(sv.symbol) == acc_.end() && acc_.size() >= MAX_SYMBOLS) {
      return;
    }
    auto& vec = acc_[sv.symbol];
    vec.push_back(sv.value);
    if (vec.size() > MAX_ACC) {
      vec.erase(vec.begin(), vec.begin() + static_cast<std::ptrdiff_t>(vec.size() - MAX_ACC));
    }
    try {
      out = fn_(Span<const ArgType>(vec.data(), vec.size()));
    } catch (const std::exception& ex) {
      // fn_ threw — pop the value we just pushed so the accumulator stays
      // consistent (the value was never processed).
      vec.pop_back();
      gma::util::logger().log(gma::util::LogLevel::Error,
                              "worker.fn_exception",
                              {{"symbol", sv.symbol}, {"err", ex.what()}});
      return;
    }
    ds = downstream_;
  }

  if (ds) {
    ds->onValue(SymbolValue{ sv.symbol, out });
  }
}

void Worker::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  downstream_.reset();
  acc_.clear();
}

} // namespace gma
