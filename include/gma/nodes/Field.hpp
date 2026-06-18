// include/gma/nodes/Field.hpp
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include "gma/nodes/INode.hpp"

namespace gma {

// Extracts one named field from a Record flowing through and forwards its
// value downstream (ENC-643). The inverse of Pack. Inputs that are not a
// Record, or Records missing the field, are dropped (nothing forwarded) — the
// same silent-drop discipline AtomicAccessor uses for absent atomics.
class Field final : public INode {
public:
  Field(std::string name, std::shared_ptr<INode> downstream);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  const std::string name_;
  std::shared_ptr<INode> downstream_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
};

} // namespace gma
