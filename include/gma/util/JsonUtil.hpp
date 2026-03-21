#pragma once

#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "gma/SymbolValue.hpp"

namespace gma::util {

// Forward declaration for mutual recursion with writeArgValueJson.
inline void writeArgTypeJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                             const gma::ArgType& v);

inline void writeArgValueJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                              const gma::ArgValue& av) {
  writeArgTypeJson(w, av.value);
}

inline void writeArgTypeJson(::rapidjson::Writer<::rapidjson::StringBuffer>& w,
                             const gma::ArgType& v) {
  std::visit([&](auto&& x) {
    using T = std::decay_t<decltype(x)>;

    if constexpr (std::is_same_v<T, bool>) {
      w.Bool(x);
    } else if constexpr (std::is_same_v<T, int>) {
      w.Int(x);
    } else if constexpr (std::is_same_v<T, double>) {
      w.Double(x);
    } else if constexpr (std::is_same_v<T, std::string>) {
      w.String(x.c_str());
    } else if constexpr (std::is_same_v<T, std::vector<int>>) {
      w.StartArray();
      for (int n : x) w.Int(n);
      w.EndArray();
    } else if constexpr (std::is_same_v<T, std::vector<double>>) {
      w.StartArray();
      for (double d : x) w.Double(d);
      w.EndArray();
    } else if constexpr (std::is_same_v<T, std::vector<gma::ArgValue>>) {
      w.StartArray();
      for (const auto& it : x) writeArgValueJson(w, it);
      w.EndArray();
    } else {
      // Fallback: unknown variant alternative
      w.Null();
    }
  }, v);
}

} // namespace gma::util
