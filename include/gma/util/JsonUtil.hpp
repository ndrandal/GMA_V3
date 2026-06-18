#pragma once

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "gma/StreamValue.hpp"

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
    } else if constexpr (std::is_same_v<T, gma::Record>) {
      // Keyed record -> JSON object, fields in insertion order (ENC-642).
      w.StartObject();
      for (const auto& f : x.fields) {
        w.Key(f.name.c_str());
        writeArgValueJson(w, f.value);
      }
      w.EndObject();
    } else {
      // Fallback: unknown variant alternative
      w.Null();
    }
  }, v);
}

// Serialize an ArgType to a JSON string (convenience for emit paths / tests).
inline std::string toJsonString(const gma::ArgType& v) {
  ::rapidjson::StringBuffer sb;
  ::rapidjson::Writer<::rapidjson::StringBuffer> w(sb);
  writeArgTypeJson(w, v);
  return std::string(sb.GetString(), sb.GetSize());
}

// Inverse of writeArgTypeJson: build an ArgType from a parsed JSON value
// (ENC-642). Objects become Records, arrays become positional ArgValue lists,
// scalars map to bool/int/double/string. JSON null maps to a default value
// (the structured types Pack/Field produce never contain nulls).
inline gma::ArgType readArgTypeJson(const ::rapidjson::Value& v) {
  if (v.IsBool())   return v.GetBool();
  if (v.IsInt())    return v.GetInt();
  if (v.IsNumber()) return v.GetDouble();
  if (v.IsString()) return std::string(v.GetString(), v.GetStringLength());

  if (v.IsArray()) {
    std::vector<gma::ArgValue> out;
    out.reserve(v.Size());
    for (const auto& e : v.GetArray())
      out.emplace_back(readArgTypeJson(e));
    return out;
  }

  if (v.IsObject()) {
    gma::Record r;
    r.fields.reserve(v.MemberCount());
    for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it)
      r.fields.push_back(gma::RecordField{
        std::string(it->name.GetString(), it->name.GetStringLength()),
        gma::ArgValue{readArgTypeJson(it->value)}});
    return r;
  }

  // null / unknown -> default (bool false).
  return false;
}

} // namespace gma::util
