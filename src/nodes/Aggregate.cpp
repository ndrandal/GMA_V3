#include "gma/nodes/Aggregate.hpp"
#include "gma/util/Logger.hpp"
#include <stdexcept>

namespace gma {

Aggregate::Aggregate(std::size_t arity, std::shared_ptr<INode> parent)
  : arity_(arity), parent_(std::move(parent))
{
  if (arity_ == 0)
    throw std::invalid_argument("Aggregate: arity must be > 0");
}

void Aggregate::onValue(const StreamValue& sv) {
  // Early-out on stopping_ is an optimization; correctness is guaranteed by
  // the mutex: if shutdown() races, the lock ensures we see parent_==nullptr.
  if (stopping_.load(std::memory_order_acquire)) return;

  std::vector<ArgType> batch;
  std::shared_ptr<INode> p;
  {
    std::lock_guard<std::mutex> lk(mx_);
    // Cap distinct symbol count to prevent unbounded map growth.
    if (buf_.find(sv.symbol) == buf_.end() && buf_.size() >= MAX_SYMBOLS) {
      gma::util::logger().log(gma::util::LogLevel::Warn,
        "Aggregate: max symbols reached, dropping",
        {{"symbol", sv.symbol}});
      return;
    }
    auto& sb = buf_[sv.symbol];
    sb.vals.push_back(sv.value);
    if (sb.vals.size() < arity_) return;
    batch = std::move(sb.vals);
    sb.vals.clear();
    p = parent_;
  }

  // Forward outside the lock to avoid holding it during downstream calls
  if (p) {
    for (const auto& v : batch) {
      // ENC-1280: the batch is released by `sv`, so every value in it is
      // forwarded under `sv`'s bucket identity.
      p->onValue(StreamValue{ sv.symbol, v, sv.bucketStartMs });
    }
  }
}

// THROWAWAY PROTOTYPE (ENC-1289 falsification only — not for merge).
// Port-indexed fan-in: a tuple completes only when EVERY declared input has
// reported, and is assembled in declared port order.
void AggPort::onValue(const StreamValue& sv) {
  if (auto p = owner_.lock()) p->onPort(idx_, sv);
}

void Aggregate::onPort(std::size_t idx, const StreamValue& sv) {
  if (stopping_.load(std::memory_order_acquire)) return;
  if (idx >= arity_) return;

  std::vector<ArgType> batch;
  std::shared_ptr<INode> p;
  {
    std::lock_guard<std::mutex> lk(mx_);
    auto it = slots_.find(sv.symbol);
    if (it == slots_.end()) {
      if (slots_.size() >= MAX_SYMBOLS) return;
      it = slots_.emplace(sv.symbol, SymSlots{}).first;
      it->second.latest.resize(arity_);
    }
    auto& st = it->second;
    if (!st.latest[idx].has_value()) ++st.filled;
    st.latest[idx] = sv.value;
    if (st.filled < arity_) return;
    batch.reserve(arity_);
    for (std::size_t i = 0; i < arity_; ++i) batch.push_back(*st.latest[i]);
    for (auto& s2 : st.latest) s2.reset();
    st.filled = 0;
    p = parent_;
  }
  if (p) {
    for (const auto& v : batch)
      p->onValue(StreamValue{ sv.symbol, v, sv.bucketStartMs });
  }
}

void Aggregate::shutdown() noexcept {
  stopping_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lk(mx_);
  buf_.clear();
  slots_.clear();
  parent_.reset();
}

} // namespace gma
