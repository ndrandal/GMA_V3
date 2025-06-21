// include/gma/Span.hpp
#pragma once

#include <cstddef>
#include <stdexcept>

namespace gma {

template <typename T>
class Span {
public:
  Span() noexcept : _data(nullptr), _size(0) {}
  Span(T* data, std::size_t size) noexcept : _data(data), _size(size) {}

  T* data() const noexcept { return _data; }
  std::size_t size() const noexcept { return _size; }
  bool empty() const noexcept { return _size == 0; }

  T& operator[](std::size_t index) const {
    if (index >= _size) throw std::out_of_range("Span index out of bounds");
    return _data[index];
  }

  T* begin() const noexcept { return _data; }
  T* end() const noexcept { return _data + _size; }

private:
  T* _data;
  std::size_t _size;
};

} // namespace gma
