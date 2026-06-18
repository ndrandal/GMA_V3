// include/gma/nodes/NodeEnv.hpp
#pragma once
#include <type_traits>
#include <variant>
#include "gma/StreamValue.hpp"
#include "gma/Expr.hpp"

namespace gma::nodes {

// Numeric coercion shared by the expression-driven nodes (Expr, Filter,
// Switch): numbers map through; non-numeric values (strings, vectors, nested
// records) collapse to 0.0 so an expression degrades gracefully.
inline double toDoubleArg(const ArgType& v) {
  return std::visit([](auto&& x) -> double {
    using T = std::decay_t<decltype(x)>;
    if constexpr (std::is_same_v<T, bool>)        return x ? 1.0 : 0.0;
    else if constexpr (std::is_same_v<T, int>)    return static_cast<double>(x);
    else if constexpr (std::is_same_v<T, double>) return x;
    else                                          return 0.0;
  }, v);
}

// Build an expression evaluation Env from an incoming value: a Record exposes
// each field by name; any scalar is exposed under the ref name "value".
inline expr::Env envFromValue(const StreamValue& sv) {
  expr::Env env;
  if (const Record* r = std::get_if<Record>(&sv.value)) {
    env.reserve(r->fields.size());
    for (const auto& f : r->fields) env[f.name] = toDoubleArg(f.value.value);
  } else {
    env.emplace("value", toDoubleArg(sv.value));
  }
  return env;
}

} // namespace gma::nodes
