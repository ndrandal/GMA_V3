// include/gma/nodes/INode.hpp
#pragma once
#include "gma/StreamValue.hpp"
#include "gma/AtomicStore.hpp"

namespace gma {

class INode {
public:
  INode() = default;
  virtual ~INode() = default;

  INode(const INode&) = delete;
  INode& operator=(const INode&) = delete;

  virtual void onValue(const StreamValue& sv) = 0;
  virtual void shutdown() noexcept = 0;
};

} // namespace gma
