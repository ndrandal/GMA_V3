#pragma once
#include <atomic>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include "gma/nodes/INode.hpp"
#include "gma/Span.hpp"

namespace gma {

// Fan-in node: collects N values per symbol before forwarding the batch
// to parent. Thread-safe — multiple upstream nodes may call onValue()
// concurrently.
class Aggregate;

// THROWAWAY PROTOTYPE (ENC-1289 falsification only — not for merge).
class AggPort final : public INode {
public:
  AggPort(std::weak_ptr<Aggregate> owner, std::size_t idx)
    : owner_(std::move(owner)), idx_(idx) {}
  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override {}
private:
  std::weak_ptr<Aggregate> owner_;
  std::size_t idx_;
};

class Aggregate final : public INode {
public:
  Aggregate(std::size_t arity, std::shared_ptr<INode> parent);

  void onValue(const StreamValue& sv) override;
  void onPort(std::size_t idx, const StreamValue& sv);
  void shutdown() noexcept override;

private:
  struct SymBuf {
    std::vector<ArgType> vals;
  };

  static constexpr std::size_t MAX_SYMBOLS = 10000;

  const std::size_t arity_;
  std::shared_ptr<INode> parent_;

  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
  std::unordered_map<std::string, SymBuf> buf_;

  // THROWAWAY PROTOTYPE: port-indexed slots, one set per correlation key.
  struct SymSlots {
    std::vector<std::optional<ArgType>> latest;
    std::size_t filled{0};
  };
  std::unordered_map<std::string, SymSlots> slots_;
};

} // namespace gma
