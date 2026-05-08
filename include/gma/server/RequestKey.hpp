#pragma once

// RequestKey — the per-subscription identifier carried on the WS
// subscribe protocol and used to key ClientSession's active_/chains_
// maps + the Responder's outbound rendering.
//
// Historically gma's WS surface was int-keyed: smoke.js (and any
// int-native consumer) sends `{key: <int>, ...}`. Embassy and any
// string-id-native consumer sends `{id: "<string>", ...}` (request IDs
// from forum's saved-scene InstructionPackages look like
// "r-NEXO-open"). This type holds either, with the variant index
// tagging which alternative is in play.
//
// The variant lives at the wire boundary only — Dispatcher,
// AtomicStore, TreeBuilder, and Listener stay key-type-agnostic
// (they route on (streamKey, field), not on the request id).
//
// See gma_v3/specs/2026-05-08-gma-string-id-subscriptions/SPEC.md
// and customer-layer/specs/2026-05-08-embassy-saved-scene-dispatch/
// DECISIONS.md ADR-001 for the rationale.

#include <functional>
#include <optional>
#include <string>
#include <variant>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace gma {
namespace server {

using RequestKey = std::variant<int, std::string>;

// Construct: explicit alternatives keep variant-construction unambiguous.
inline RequestKey requestKeyInt(int v) { return RequestKey{std::in_place_index<0>, v}; }
inline RequestKey requestKeyStr(std::string v) {
  return RequestKey{std::in_place_index<1>, std::move(v)};
}

inline bool isInt(const RequestKey& k) { return k.index() == 0; }
inline bool isStr(const RequestKey& k) { return k.index() == 1; }

// writeRequestKeyJSON: emit the appropriate field+value pair on a
// rapidjson Writer. Int subscriptions get `"key": <int>`; string
// subscriptions get `"requestId": "<string>"`. The asymmetry is
// deliberate — embassy's inboundProbe reads `requestId` (matching
// the existing pattern across embassy's wire types); smoke.js reads
// `key`. See SPEC.md "Outgoing JSON field naming for string-keyed
// subs" — option (W).
template <typename Writer>
inline void writeRequestKeyJSON(Writer& w, const RequestKey& k) {
  if (isInt(k)) {
    w.Key("key");
    w.Int(std::get<int>(k));
  } else {
    w.Key("requestId");
    w.String(std::get<std::string>(k).c_str());
  }
}

// parseRequestKeyFromObj: read either `key: <int>`, `id: <int>`
// (legacy fallback), or `id: "<string>"` from a request object on the
// subscribe wire. Returns std::nullopt if neither field is present
// in a valid form. A request that supplies BOTH `key` AND `id` is
// rejected — caller is expected to check this first (sendError).
inline std::optional<RequestKey>
parseRequestKeyFromObj(const rapidjson::Value& r) {
  const bool hasKey = r.HasMember("key");
  const bool hasId  = r.HasMember("id");

  // Mutual exclusivity: both present is a protocol error. Caller
  // reports it; we just signal "no valid key" here.
  if (hasKey && hasId) return std::nullopt;

  if (hasKey) {
    if (!r["key"].IsInt()) return std::nullopt;
    return requestKeyInt(r["key"].GetInt());
  }
  if (hasId) {
    if (r["id"].IsInt()) return requestKeyInt(r["id"].GetInt());
    if (r["id"].IsString()) return requestKeyStr(r["id"].GetString());
  }
  return std::nullopt;
}

// requestObjHasBothKeyAndId: caller-side helper for the
// mutual-exclusivity check, because parseRequestKeyFromObj() returns
// nullopt for "valid neither" and "both present" — distinguishing
// the two is what the caller wants to error on.
inline bool requestObjHasBothKeyAndId(const rapidjson::Value& r) {
  return r.HasMember("key") && r.HasMember("id");
}

// keyDebugString: human-readable rendering for log lines. Int subs
// get "key=N"; string subs get "requestId=..." (mirrors the wire
// rendering so logs and frames line up).
inline std::string keyDebugString(const RequestKey& k) {
  if (isInt(k)) return "key=" + std::to_string(std::get<int>(k));
  return "requestId=" + std::get<std::string>(k);
}

}  // namespace server
}  // namespace gma

// std::hash specialization so unordered_map<RequestKey, ...> works.
// Salts on the variant index so int(5) and string("5") hash to
// different buckets — catching the obvious silent-collision bug.
namespace std {
template <>
struct hash<gma::server::RequestKey> {
  std::size_t operator()(const gma::server::RequestKey& k) const noexcept {
    const std::size_t idx = static_cast<std::size_t>(k.index());
    if (idx == 0) {
      return std::hash<int>{}(std::get<int>(k)) ^ (idx << 32);
    }
    return std::hash<std::string>{}(std::get<std::string>(k)) ^ ((idx + 1) << 32);
  }
};
}  // namespace std
