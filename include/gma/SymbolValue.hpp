#pragma once

#include <string>
#include <variant>
#include <vector>

namespace gma {

// Forward declare the wrapper struct first
struct ArgValue;

// Define ArgType using ArgValue (not recursively itself)
using ArgType = std::variant<
  bool,
  int,
  double,
  std::string,
  std::vector<int>,
  std::vector<double>,
  std::vector<ArgValue>  
>;

// Define the wrapper struct
struct ArgValue {
  ArgType value;

  ArgValue() = default;
  ArgValue(const ArgType& val) : value(val) {}
  ArgValue(ArgType&& val) : value(std::move(val)) {}

  // Optional: enable implicit conversion from any ArgType alternative
  template <typename T,
            typename = std::enable_if_t<!std::is_same_v<std::decay_t<T>, ArgValue>>>
  ArgValue(T&& val) : value(std::forward<T>(val)) {}
};

// Core value for computation
struct SymbolValue {
  std::string symbol;
  ArgType value;
};

} // namespace gma
