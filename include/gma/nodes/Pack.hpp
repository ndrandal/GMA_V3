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

namespace gma {

class Pack;

// Per-field input adapter: tags each incoming value with its field index and
// forwards it to the owning Pack. Holds a weak_ptr so it never keeps Pack alive
// (Pack owns the ports; ports only weak-reference Pack — no ownership cycle).
class PackPort final : public INode {
public:
  PackPort(std::weak_ptr<Pack> owner, std::size_t idx);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override {}

private:
  std::weak_ptr<Pack> owner_;
  std::size_t idx_;
};

// Fan-in node that assembles N named inputs into a keyed Record per symbol
// (combineLatest semantics, ENC-643): once every field has produced at least
// one value for a symbol, each subsequent field update emits a Record of the
// latest per-field values, in declared field order. The inverse of Field.
//
// Bounded (total invariant): a fixed field count and a capped per-symbol state
// map; each emit does O(fields) work. Mirrors Aggregate's fan-in ownership —
// the builder returns a CompositeRoot holding the input heads plus the Pack.
class Pack final : public INode, public std::enable_shared_from_this<Pack> {
public:
  Pack(std::vector<std::string> names, std::shared_ptr<INode> downstream);

  // Fan-in: fed via onField() from the ports, never as a pipeline sink.
  void onValue(const StreamValue&) override {}
  void shutdown() noexcept override;

  void onField(std::size_t idx, const StreamValue& sv);
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
