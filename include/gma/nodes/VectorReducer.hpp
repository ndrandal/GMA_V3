#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include "gma/FunctionMap.hpp"   // for gma::Func = double(const vector<double>&)
#include "gma/nodes/INode.hpp"

namespace gma {

// Receives one `StreamValue{symbol, vector<double>}` per upstream emit,
// applies `fn_` to the held vector, and forwards one
// `StreamValue{symbol, double}` downstream. The companion to
// `TumblingWindow` (which emits the vector); together they express
// "reduce a stream over a wall-clock period."
//
// `fn_` is a `gma::Func = std::function<double(const std::vector<double>&)>`
// resolved at build time from the shared `FunctionMap` registry — the same
// registry `Worker` uses (Worker just adapts the variant signature in its
// builder; `VectorReducer` consumes the native shape directly).
//
// Inputs whose `StreamValue::value` variant does not hold a
// `std::vector<double>` are silently dropped with a single `Warn` log.
// Reducer exceptions are caught and logged at `Error`; the value is
// dropped and state stays consistent (mirror of `Worker.cpp`'s pattern).
class VectorReducer final : public INode {
public:
  VectorReducer(Func fn, std::shared_ptr<INode> downstream);

  void onValue(const StreamValue& sv) override;
  void shutdown() noexcept override;

private:
  Func fn_;
  std::shared_ptr<INode> downstream_;

  std::atomic<bool> stopping_{false};
  mutable std::mutex mx_;
};

} // namespace gma
