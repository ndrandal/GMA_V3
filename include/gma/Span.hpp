#pragma once
#include <cstddef>
#include <type_traits>

namespace gma_detail {

// Minimal, header-only span for C++17 (read-only)
template <class T>
class basic_span {
public:
  using element_type = T;
  using value_type   = typename std::remove_cv<T>::type;
  using size_type    = std::size_t;
  using pointer      = const T*;
  using iterator     = const T*;

  basic_span() : p_(nullptr), n_(0) {}
  basic_span(const T* p, size_type n) : p_(p), n_(n) {}

  iterator begin() const { return p_; }
  iterator end()   const { return p_ + n_; }
  pointer  data()  const { return p_; }
  size_type size() const { return n_; }
  bool empty()     const { return n_ == 0; }
  const T& operator[](size_type i) const { return p_[i]; }

private:
  const T* p_;
  size_type n_;
};

} // namespace gma_detail

namespace gma {
// Forward declare ArgType so this header can be included almost anywhere.
struct ArgType_fwd_tag;
} // namespace gma

namespace gma {

// Authoritative Span alias template
template <class T>
using Span = gma_detail::basic_span<T>;

// After ArgType is known, other headers may do:
//   using ArgSpan = Span<const ArgType>;
//   using Span_t  = ArgSpan;
} // namespace gma
