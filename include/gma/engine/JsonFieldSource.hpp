#pragma once
//
// JsonFieldSource — the JSON DOM adapter for FieldSource (ENC-1486).
//
// Design: specs/2026-10-14-gma-data-layer/SPEC.md D1; docs/data-layer.md §C2.
//
// This is the LEGACY ingress shape and it is unchanged: the TCP feed server
// and `market.wsclient` both hand `Dispatcher::onTick` a
// `shared_ptr<rapidjson::Document>`, and this class is the thin wrapper that
// lets `onTick` share one body with `onRecord`.
//
// BYTE-FOR-BYTE WITH THE PRE-ENC-1486 READS, deliberately, down to details
// that look like mistakes:
//
//   * `has()` and the getters look members up by `name.c_str()`, not by
//     (pointer, length). `onTick` has always done `HasMember(field.c_str())`,
//     so a subscribed field whose name contains an embedded NUL has always
//     been truncated at it. Matching that is the point — this ticket is not
//     the place to change which fields resolve.
//   * `tryGetUint64` requires `IsUint64()`, not merely an integral double.
//     `MarketTickComputer`'s timestamp read did exactly that.
//   * `forEachNumber` visits members in DOM (insertion) order and skips any
//     member whose name is not a string or whose value is not a number —
//     the pre-existing raw-admission filter, verbatim.
//
// THE ONE DIFFERENCE, and it is a hardening with no reachable behaviour
// change: every accessor is guarded on `IsObject()`. rapidjson's `HasMember`
// asserts `IsObject()`, so before this a non-object payload reaching a
// symbol with a live subscription aborted the process inside rapidjson. It is
// now an empty field set. No test covered that path, nothing in the shipped
// ingress can produce it, and an abort was not a behaviour worth preserving.
//

#include <cstdint>
#include <string>

#include <rapidjson/document.h>

#include "gma/engine/FieldSource.hpp"

namespace gma::engine {

class JsonFieldSource final : public FieldSource {
public:
  explicit JsonFieldSource(const rapidjson::Value& payload) noexcept
    : payload_(&payload) {}

  // NULL-ACCEPTING OVERLOAD, for `IEventComputer::computeEvent`: an `Event`
  // with no payload at all is an empty field set, not a skipped computer.
  // That keeps the pre-ENC-1486 behaviour of driving a computer directly with
  // a default-constructed `Event` — the computer WAS entered and bailed on
  // `!e.payload` itself (tests/engine/RegistriesTest.cpp counts exactly that),
  // so swallowing the call here would have changed what "invoked" means.
  explicit JsonFieldSource(const rapidjson::Value* payload) noexcept
    : payload_(payload) {}

  bool isObject() const noexcept override {
    return payload_ != nullptr && payload_->IsObject();
  }

  std::size_t fieldCountHint() const noexcept override {
    return isObject() ? payload_->MemberCount() : 0u;
  }

  bool has(const std::string& name) const override {
    if (!isObject()) return false;
    auto it = payload_->FindMember(name.c_str());
    return it != payload_->MemberEnd() && it->value.IsNumber();
  }

  bool tryGetNumber(const std::string& name, double& out) const override {
    if (!isObject()) return false;
    auto it = payload_->FindMember(name.c_str());
    if (it == payload_->MemberEnd() || !it->value.IsNumber()) return false;
    out = it->value.GetDouble();
    return true;
  }

  bool tryGetUint64(const std::string& name, std::uint64_t& out) const override {
    if (!isObject()) return false;
    auto it = payload_->FindMember(name.c_str());
    if (it == payload_->MemberEnd() || !it->value.IsUint64()) return false;
    out = it->value.GetUint64();
    return true;
  }

  void forEachNumber(NumericFieldSink& sink) const override {
    if (!isObject()) return;
    // ONE reused buffer, not a fresh `std::string` per member. The pre-ENC-1486
    // loop constructed a `std::string name(it->name.GetString(), ...)` per
    // member per tick — the cost docs/data-layer.md §C6 names — and the sink
    // copies the name anyway when it admits it, so constructing a second one
    // here would be pure waste. `assign` reuses the capacity, so a tick costs
    // at most one allocation for names rather than one per member.
    std::string name;
    for (auto it = payload_->MemberBegin(); it != payload_->MemberEnd(); ++it) {
      if (!it->name.IsString() || !it->value.IsNumber()) continue;
      name.assign(it->name.GetString(), it->name.GetStringLength());
      sink.field(name, it->value.GetDouble());
    }
  }

private:
  const rapidjson::Value* payload_;
};

} // namespace gma::engine
