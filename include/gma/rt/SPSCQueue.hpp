#pragma once
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <vector>
#include <optional>

namespace gma::rt {

// Single-Producer Single-Consumer ring buffer (bounded, wait-free).
template <typename T>
class SPSCQueue {
public:
  explicit SPSCQueue(size_t capacity)
  : cap_(capacity), buf_(capacity) {
    if (capacity == 0)
      throw std::invalid_argument("SPSCQueue: capacity must be > 0");
  }

  // Try to push; returns false if full.
  bool try_push(const T& v) {
    size_t h = head_.load(std::memory_order_relaxed);
    size_t n = (h + 1) % cap_;
    if (n == tail_.load(std::memory_order_acquire)) return false; // full
    buf_[h] = v;
    head_.store(n, std::memory_order_release);
    return true;
  }

  // Pop one; returns empty optional if empty.
  std::optional<T> try_pop() {
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return std::nullopt; // empty
    auto out = std::move(buf_[t]);
    tail_.store((t + 1) % cap_, std::memory_order_release);
    return out;
  }

  // Drain up to max items (0 = all available).
  template <typename Fn>
  size_t drain(Fn&& fn, size_t max=0) {
    size_t cnt = 0;
    while (true) {
      auto item = try_pop();
      if (!item) break;
      fn(std::move(*item));
      if (max && ++cnt >= max) break;
    }
    return cnt;
  }

  bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
  bool full()  const { size_t n = (head_.load() + 1) % cap_; return n == tail_.load(); }
  size_t cap() const { return cap_; }

  // Drop one (oldest); use for backpressure strategy.
  bool drop_one() {
    size_t t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false; // empty
    tail_.store((t + 1) % cap_, std::memory_order_release);
    return true;
  }

private:
  const size_t cap_;
  std::vector<T> buf_;
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

} // namespace gma::rt
