#pragma once

#include <string>
#include <variant>

namespace gma {

struct Error {
  std::string message;
  std::string path;

  std::string describe() const {
    return path + ": " + message;
  }
};

template <typename T>
class Result {
public:
  Result(const T& value) : _value(value) {}
  Result(T&& value) : _value(std::move(value)) {}
  Result(const Error& error) : _value(error) {}
  Result(Error&& error) : _value(std::move(error)) {}

  bool has_value() const { return std::holds_alternative<T>(_value); }
  explicit operator bool() const { return has_value(); }

  T& value() { return std::get<T>(_value); }
  const T& value() const { return std::get<T>(_value); }

  const Error& error() const { return std::get<Error>(_value); }

  T& operator*() { return value(); }
  const T& operator*() const { return value(); }

private:
  std::variant<T, Error> _value;
};

} // namespace gma
