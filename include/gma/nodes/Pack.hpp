// include/gma/nodes/Pack.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "gma/nodes/INode.hpp"
#include "gma/nodes/InputPort.hpp"

namespace gma {

class Pack;

// ENC-1291 (SPEC D2): `PackPort` was the adapter that made Pack the one fan-in
// in the engine that joined by INPUT rather than by value count. It has been
// generalized into `gma::InputPort` + `gma::IFanIn` (include/gma/nodes/
// InputPort.hpp) and is now shared with `Aggregate`. The ownership direction is
// unchanged and still load-bearing: Pack owns its ports, each port holds only a
// weak_ptr back. No alias is left behind on purpose: a port whose index means
// "field" and a port whose index means "input" are the same object now, and two
// spellings for it is how the two fan-ins drifted apart in the first place.

// Fan-in node that assembles N named inputs into a keyed Record per symbol
// (combineLatest semantics, ENC-643): once every field has produced at least
// one value for a symbol, each subsequent field update emits a Record of the
// latest per-field values, in declared field order. The inverse of Field.
//
// Bounded (total invariant): a fixed field count and a capped per-symbol state
// map; each emit does O(fields) work. Mirrors Aggregate's fan-in ownership —
// the builder returns a CompositeRoot holding the input heads plus the Pack.
class Pack final : public INode, public IFanIn {
public:
  Pack(std::vector<std::string> names, std::shared_ptr<INode> downstream);

  // Fan-in: fed via onPortValue() from the ports, never as a pipeline sink.
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override;

  // ENC-1291: the IFanIn half of the port contract. `idx` is the field's
  // position in the declared `fields` object, assigned at build time.
  void onPortValue(std::size_t idx, const StreamValue& sv) override;
  std::size_t fieldCount() const noexcept { return names_.size(); }

  // Builder helper: take ownership of a constructed port so it outlives the
  // upstream Listener's weak_ptr to it.
  void addPort(std::shared_ptr<INode> port);

private:
  struct SymState {
    std::vector<std::optional<ArgValue>> latest;
    std::size_t filled = 0;
  };

  static constexpr std::size_t MAX_SYMBOLS = 10000;

  const std::vector<std::string> names_;
  std::shared_ptr<INode> downstream_;
  std::vector<std::shared_ptr<INode>> ports_;

  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  std::unordered_map<std::string, SymState> state_;
};

} // namespace gma
